#include <stdio.h>
#include <stdlib.h>

const unsigned int numberOfBits   = 65536;
unsigned int buffer[65536] = {0};


main(int argc, char* argv[])
{
  unsigned int counter;
  FILE* file;

  file = fopen(argv[1], "r");
  for(counter = 0; counter < numberOfBits; counter++) {
    fscanf(file, "%X\n", &buffer[counter]);
  }
  fclose(file);

  file = fopen(argv[2], "w");
  fwrite(buffer, numberOfBits, sizeof(unsigned int), file);
  fclose(file);
}
