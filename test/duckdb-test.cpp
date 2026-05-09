#include <duckdb.hpp>

#include <cstdlib>
#include <iostream>

int main()
{
    try {
        auto db = duckdb::DuckDB{nullptr};
        auto connection = duckdb::Connection{db};
        auto result = connection.Query("select 1 + 1");
        if (result->HasError()) {
            std::cerr << result->GetError() << '\n';
            return EXIT_FAILURE;
        }
        if (result->RowCount() != 1 || result->GetValue(0, 0).GetValue<int>() != 2)
            return EXIT_FAILURE;
    } catch (const std::exception & e) {
        std::cerr << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
