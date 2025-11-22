#include <cstdio>
#include <cstdlib>
#include <iostream>

int
main ()
{
  // Clear screen and move cursor to home.
  std::cout << "\x1b[2J";
  std::cout << "\x1b[H";

  std::cout << "HELLO\r\n";
  std::cout << "WORLD!!";

  // Move cursor to row 4, column 8 (1-based).
  std::cout << "\x1b[4;8H";
  std::cout.flush ();
  return EXIT_SUCCESS;
}
