// cgroups v2 parser and scanner

#include <algorithm>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/color.h>
#include <fmt/std.h>
#include <fstream>
#include <map>
#include <mp-units/format.h>
#include <mp-units/math.h>
#include <mp-units/ostream.h>
#include <mp-units/systems/iec.h>
#include <mp-units/systems/si.h>
#include <sstream>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace nxb::cgroups
{
using fmt::print;
using namespace mp_units;
using namespace mp_units::iec::unit_symbols;
using namespace mp_units::si::unit_symbols;

// Define custom page unit (standard 4 KiB page)
inline constexpr struct page final : named_unit<"page", mag<4096> * iec::byte>
{
} page;

// Variant to hold different quantity types
using stat_value = std::variant<decltype (1.0 * B), decltype (1.0 * page),
                                decltype (1.0 * us), decltype (1.0 * percent),
                                std::uint64_t>; // dimensionless counts

// Field metadata with unit type
struct field_info
{
  std::string_view name;
  std::string_view label;
  enum class unit_type
  {
    bytes,
    pages,
    microseconds,
    percent,
    count
  } unit;
};

// memory.stat field encyclopedia
constexpr std::array memory_stat_fields = {
  field_info{ "anon", "non-file data", field_info::unit_type::bytes },
  field_info{ "file", "cached file data (total)", field_info::unit_type::bytes },
  field_info{ "kernel", "kernel data", field_info::unit_type::bytes },
  field_info{ "kernel_stack", "kernel stack data", field_info::unit_type::bytes },
  field_info{ "pagetables", "page table data", field_info::unit_type::bytes },
  field_info{ "slab", "cached kernel objects", field_info::unit_type::bytes },
  field_info{ "slab_reclaimable", "cached kernel objects (reclaimable)",
              field_info::unit_type::bytes },
  field_info{ "slab_unreclaimable", "cached kernel objects (unreclaimable)",
              field_info::unit_type::bytes },
  field_info{ "sock", "socket buffer data", field_info::unit_type::bytes },
  field_info{ "vmalloc", "virtually allocated data",
              field_info::unit_type::bytes },
  field_info{ "shmem", "shared memory data", field_info::unit_type::bytes },
  field_info{ "file_mapped", "mapped file data",
              field_info::unit_type::pages },
  field_info{ "file_dirty", "dirty file data", field_info::unit_type::pages },
  field_info{ "file_writeback", "file data (writeback queue)",
              field_info::unit_type::pages },
  field_info{ "swapcached", "cached swap data", field_info::unit_type::bytes },
  field_info{ "anon_thp", "non-file data (transparent huge pages)",
              field_info::unit_type::bytes },
  field_info{ "file_thp", "file data (transparent huge pages)",
              field_info::unit_type::bytes },
  field_info{ "inactive_anon", "non-file data (inactive)",
              field_info::unit_type::bytes },
  field_info{ "active_anon", "non-file data (active)",
              field_info::unit_type::bytes },
  field_info{ "inactive_file", "cached file data (inactive)",
              field_info::unit_type::bytes },
  field_info{ "active_file", "cached file data (active)",
              field_info::unit_type::bytes },
  field_info{ "unevictable", "protected data",
              field_info::unit_type::bytes },
  field_info{ "pgfault", "memory access fault events",
              field_info::unit_type::count },
  field_info{ "pgmajfault", "memory access fault events (disk-backed)",
              field_info::unit_type::count },
  field_info{ "workingset_refault_anon",
              "working set refault events (non-file)",
              field_info::unit_type::count },
  field_info{ "workingset_refault_file",
              "working set refault events (file)",
              field_info::unit_type::count },
  field_info{ "workingset_activate_anon",
              "working set promotion events (non-file)",
              field_info::unit_type::count },
  field_info{ "workingset_activate_file",
              "working set promotion events (file)",
              field_info::unit_type::count },
  field_info{ "workingset_restore_anon",
              "working set restoration events (non-file)",
              field_info::unit_type::count },
  field_info{ "workingset_restore_file",
              "working set restoration events (file)",
              field_info::unit_type::count },
  field_info{ "workingset_nodereclaim", "working set node reclaim events",
              field_info::unit_type::count },
  field_info{ "pgscan", "memory reclaim scan events",
              field_info::unit_type::count },
  field_info{ "pgsteal", "memory reclaim eviction events",
              field_info::unit_type::count },
  field_info{ "pgscan_kswapd", "memory reclaim scan events (kswapd)",
              field_info::unit_type::count },
  field_info{ "pgscan_direct", "memory reclaim scan events (direct)",
              field_info::unit_type::count },
  field_info{ "pgsteal_kswapd", "memory reclaim eviction events (kswapd)",
              field_info::unit_type::count },
  field_info{ "pgsteal_direct", "memory reclaim eviction events (direct)",
              field_info::unit_type::count },
  field_info{ "pgactivate", "cache promotion events",
              field_info::unit_type::count },
  field_info{ "pgdeactivate", "cache demotion events",
              field_info::unit_type::count },
  field_info{ "pglazyfree", "lazy free events (scheduled)",
              field_info::unit_type::count },
  field_info{ "pglazyfreed", "lazy free events (completed)",
              field_info::unit_type::count },
  field_info{ "thp_fault_alloc",
              "transparent huge page allocation events (fault)",
              field_info::unit_type::count },
  field_info{ "thp_collapse_alloc",
              "transparent huge page allocation events (collapse)",
              field_info::unit_type::count },
};

// cpu.stat field encyclopedia
constexpr std::array cpu_stat_fields = {
  field_info{ "usage_usec", "total processor time",
              field_info::unit_type::microseconds },
  field_info{ "user_usec", "user mode time",
              field_info::unit_type::microseconds },
  field_info{ "system_usec", "kernel mode time",
              field_info::unit_type::microseconds },
  field_info{ "nr_periods", "enforcement periods",
              field_info::unit_type::count },
  field_info{ "nr_throttled", "throttled periods",
              field_info::unit_type::count },
  field_info{ "throttled_usec", "time spent throttled",
              field_info::unit_type::microseconds },
  field_info{ "nr_bursts", "burst periods", field_info::unit_type::count },
  field_info{ "burst_usec", "time spent in burst",
              field_info::unit_type::microseconds },
  field_info{ "core_sched.force_idle_usec",
              "forced idle time from core scheduling",
              field_info::unit_type::microseconds },
};

// io.stat field encyclopedia (per-device fields)
constexpr std::array io_stat_fields = {
  field_info{ "rbytes", "data read", field_info::unit_type::bytes },
  field_info{ "wbytes", "data written", field_info::unit_type::bytes },
  field_info{ "rios", "read operations", field_info::unit_type::count },
  field_info{ "wios", "write operations", field_info::unit_type::count },
  field_info{ "dbytes", "data discarded", field_info::unit_type::bytes },
  field_info{ "dios", "discard operations", field_info::unit_type::count },
};

// pressure stat field encyclopedia (for cpu.pressure, io.pressure,
// memory.pressure)
constexpr std::array pressure_fields = {
  field_info{ "some avg10", "some tasks stalled ten second average",
              field_info::unit_type::percent },
  field_info{ "some avg60", "some tasks stalled sixty second average",
              field_info::unit_type::percent },
  field_info{ "some avg300", "some tasks stalled five minute average",
              field_info::unit_type::percent },
  field_info{ "some total", "some tasks stalled total time",
              field_info::unit_type::microseconds },
  field_info{ "full avg10", "all tasks stalled ten second average",
              field_info::unit_type::percent },
  field_info{ "full avg60", "all tasks stalled sixty second average",
              field_info::unit_type::percent },
  field_info{ "full avg300", "all tasks stalled five minute average",
              field_info::unit_type::percent },
  field_info{ "full total", "all tasks stalled total time",
              field_info::unit_type::microseconds },
};

// Parse value based on unit type
stat_value
parse_value (std::uint64_t raw, field_info::unit_type unit)
{
  switch (unit)
    {
    case field_info::unit_type::bytes:
      return static_cast<double> (raw) * B;
    case field_info::unit_type::pages:
      return static_cast<double> (raw) * page;
    case field_info::unit_type::microseconds:
      return static_cast<double> (raw) * us;
    case field_info::unit_type::percent:
      return static_cast<double> (raw) * percent;
    case field_info::unit_type::count:
      return raw;
    }
  std::unreachable ();
}

// Format value with nice units
std::string
format_value (const stat_value &value)
{
  return std::visit (
      [] (auto &&val) -> std::string {
        using T = std::decay_t<decltype (val)>;
        if constexpr (std::is_same_v<T, std::uint64_t>)
          {
            return fmt::format ("{}", val);
          }
        else if constexpr (requires { val.in (GiB); })
          {
            // Byte/page quantities - convert to bytes first for comparison
            auto val_in_bytes = val.in (B);
            auto val_numeric = val_in_bytes.numerical_value_ref_in (B);
            constexpr auto GiB_in_bytes = 1073741824.0; // 2^30
            constexpr auto MiB_in_bytes = 1048576.0;    // 2^20
            constexpr auto KiB_in_bytes = 1024.0;       // 2^10

            if (val_numeric >= GiB_in_bytes)
              return fmt::format ("{::N[.2f]}", val_in_bytes.in (GiB));
            else if (val_numeric >= MiB_in_bytes)
              return fmt::format ("{::N[.2f]}", val_in_bytes.in (MiB));
            else if (val_numeric >= KiB_in_bytes)
              return fmt::format ("{::N[.2f]}", val_in_bytes.in (KiB));
            else
              return fmt::format ("{::N[.0f]}", val_in_bytes);
          }
        else if constexpr (requires { val.in (h); })
          {
            // Time quantities - smart formatting using mp-units
            auto val_numeric = val.numerical_value_ref_in (us);

            if (val_numeric >= 3600000000UL) // 1 hour in microseconds
              return fmt::format ("{::N[.2f]}", val.in (h));
            else if (val_numeric >= 60000000UL) // 1 minute in microseconds
              return fmt::format ("{::N[.2f]}", val.in (min));
            else if (val_numeric >= 1000000UL) // 1 second in microseconds
              return fmt::format ("{::N[.2f]}", val.in (s));
            else if (val_numeric >= 1000UL) // 1 ms in microseconds
              return fmt::format ("{::N[.2f]}", val.in (ms));
            else
              return fmt::format ("{}", val);
          }
        else
          {
            return fmt::format ("{}", val);
          }
      },
      value);
}

// Parse memory.stat file
std::map<std::string, stat_value>
parse_memory_stat (const std::filesystem::path &path)
{
  std::map<std::string, stat_value> stats;
  std::ifstream file (path);
  std::string line;

  while (std::getline (file, line))
    {
      std::istringstream iss (line);
      std::string key;
      std::uint64_t value;

      if (iss >> key >> value)
        {
          // Find the field info for this key
          for (const auto &field : memory_stat_fields)
            {
              if (field.name == key)
                {
                  stats[key] = parse_value (value, field.unit);
                  break;
                }
            }
        }
    }
  return stats;
}

// Parse cpu.stat file (same format as memory.stat)
std::map<std::string, stat_value>
parse_cpu_stat (const std::filesystem::path &path)
{
  std::map<std::string, stat_value> stats;
  std::ifstream file (path);
  std::string line;

  while (std::getline (file, line))
    {
      std::istringstream iss (line);
      std::string key;
      std::uint64_t value;

      if (iss >> key >> value)
        {
          // Find the field info for this key
          for (const auto &field : cpu_stat_fields)
            {
              if (field.name == key)
                {
                  stats[key] = parse_value (value, field.unit);
                  break;
                }
            }
        }
    }
  return stats;
}

// Parse io.stat file (device_id key=value format)
struct io_device_stats
{
  std::string device_id;
  std::map<std::string, stat_value> stats;
};

std::vector<io_device_stats>
parse_io_stat (const std::filesystem::path &path)
{
  std::vector<io_device_stats> devices;
  std::ifstream file (path);
  std::string line;

  while (std::getline (file, line))
    {
      std::istringstream iss (line);
      io_device_stats device;

      // First token is device id (e.g., "259:1")
      if (!(iss >> device.device_id))
        continue;

      // Rest are key=value pairs
      std::string pair;
      while (iss >> pair)
        {
          auto eq_pos = pair.find ('=');
          if (eq_pos != std::string::npos)
            {
              std::string key = pair.substr (0, eq_pos);
              std::uint64_t value = std::stoull (pair.substr (eq_pos + 1));

              // Find the field info
              for (const auto &field : io_stat_fields)
                {
                  if (field.name == key)
                    {
                      device.stats[key] = parse_value (value, field.unit);
                      break;
                    }
                }
            }
        }
      devices.push_back (device);
    }
  return devices;
}

void
scan ()
{
  std::filesystem::path cgroup_path
      = "/sys/fs/cgroup/system.slice/nix-daemon.service";

  // Parse and display memory.stat
  auto memory_stat_path = cgroup_path / "memory.stat";
  if (std::filesystem::exists (memory_stat_path))
    {
      print ("{}\n",
             fmt::styled ("memory.stat", fmt::fg (fmt::color::light_steel_blue)
                                             | fmt::emphasis::bold));

      auto stats = parse_memory_stat (memory_stat_path);

      // Collect fields with their values and labels
      std::vector<std::tuple<std::string_view, std::string>> rows;
      for (const auto &field : memory_stat_fields)
        {
          if (auto it = stats.find (std::string (field.name));
              it != stats.end ())
            {
              rows.emplace_back (field.label, format_value (it->second));
            }
        }

      // Sort by label
      std::sort (rows.begin (), rows.end (),
                 [] (const auto &a, const auto &b) {
                   return std::get<0> (a) < std::get<0> (b);
                 });

      // Find max label width for alignment
      size_t max_label = 0;
      for (const auto &[label, value] : rows)
        max_label = std::max (max_label, label.size ());

      // Display sorted and aligned
      for (const auto &[label, value] : rows)
        {
          print ("  {:<{}}  {}\n",
                 fmt::styled (label, fmt::fg (fmt::color::lawn_green)),
                 max_label,
                 fmt::styled (value, fmt::fg (fmt::color::golden_rod)
                                         | fmt::emphasis::bold));
        }
      print ("\n");
    }

  // Parse and display cpu.stat
  auto cpu_stat_path = cgroup_path / "cpu.stat";
  if (std::filesystem::exists (cpu_stat_path))
    {
      print ("{}\n",
             fmt::styled ("cpu.stat", fmt::fg (fmt::color::light_steel_blue)
                                          | fmt::emphasis::bold));

      auto stats = parse_cpu_stat (cpu_stat_path);

      // Collect and sort
      std::vector<std::tuple<std::string_view, std::string>> rows;
      for (const auto &field : cpu_stat_fields)
        {
          if (auto it = stats.find (std::string (field.name));
              it != stats.end ())
            {
              rows.emplace_back (field.label, format_value (it->second));
            }
        }

      std::sort (rows.begin (), rows.end (),
                 [] (const auto &a, const auto &b) {
                   return std::get<0> (a) < std::get<0> (b);
                 });

      size_t max_label = 0;
      for (const auto &[label, value] : rows)
        max_label = std::max (max_label, label.size ());

      for (const auto &[label, value] : rows)
        {
          print ("  {:<{}}  {}\n",
                 fmt::styled (label, fmt::fg (fmt::color::lawn_green)),
                 max_label,
                 fmt::styled (value, fmt::fg (fmt::color::golden_rod)
                                         | fmt::emphasis::bold));
        }
      print ("\n");
    }

  // Parse and display io.stat
  auto io_stat_path = cgroup_path / "io.stat";
  if (std::filesystem::exists (io_stat_path))
    {
      print ("{}\n",
             fmt::styled ("io.stat", fmt::fg (fmt::color::light_steel_blue)
                                         | fmt::emphasis::bold));

      auto devices = parse_io_stat (io_stat_path);

      for (const auto &device : devices)
        {
          print ("  {} {}\n",
                 fmt::styled ("device", fmt::fg (fmt::color::orchid)),
                 fmt::styled (device.device_id, fmt::fg (fmt::color::sky_blue)
                                                    | fmt::emphasis::bold));

          // Collect and sort
          std::vector<std::tuple<std::string_view, std::string>> rows;
          for (const auto &field : io_stat_fields)
            {
              if (auto it = device.stats.find (std::string (field.name));
                  it != device.stats.end ())
                {
                  rows.emplace_back (field.label, format_value (it->second));
                }
            }

          std::sort (rows.begin (), rows.end (),
                     [] (const auto &a, const auto &b) {
                       return std::get<0> (a) < std::get<0> (b);
                     });

          size_t max_label = 0;
          for (const auto &[label, value] : rows)
            max_label = std::max (max_label, label.size ());

          for (const auto &[label, value] : rows)
            {
              print ("    {:<{}}  {}\n",
                     fmt::styled (label, fmt::fg (fmt::color::lawn_green)),
                     max_label,
                     fmt::styled (value, fmt::fg (fmt::color::golden_rod)
                                             | fmt::emphasis::bold));
            }
          print ("\n");
        }
    }

  // Display other files without parsing
  constexpr std::array other_files = {
    "io.pressure",
    "cgroup.procs",
    "pids.current",
    "cpu.pressure",
  };

  for (const auto &filename : other_files)
    {
      auto file_path = cgroup_path / filename;
      if (!std::filesystem::exists (file_path))
        continue;

      auto content = std::ifstream (file_path);
      if (content.is_open ())
        {
          print ("{}\n", fmt::styled (filename,
                                      fmt::fg (fmt::color::light_steel_blue)));
          std::string line;
          while (std::getline (content, line))
            {
              print ("  {}\n",
                     fmt::styled (line, fmt::fg (fmt::color::golden_rod)
                                            | fmt::emphasis::faint));
            }
          print ("\n");
        }
    }
}

}

int
main ()
{
  nxb::cgroups::scan ();
}
