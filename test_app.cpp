#include <iostream>
#include <fcntl.h>
#include <unistd.h>

int main (int argc, char* argv[])
{
   if (argc != 2)
   {
      std::cout << "Please provide exactly one argument (path to /dev)" << std::endl;
      return -1;
   }

   std::cout << "Trying to read device " << std::string(argv[1]) << std::endl;

   int fd = open(argv[1], O_RDONLY);
   if (fd < 0)
   {
      std::cout << "cannot open device!" << std::endl;
      return fd;
   }

   char bytes [2];
   int result =  read(fd, bytes, 2);
   if (result != 2)
   {
      std::cout << "Cannot read data from device, error " << result << "!" << std::endl;
      close(fd);
      return result;
   }

   int distance = bytes[1];
   distance |= (bytes[0] << 8);
   std::cout << "distance: " << distance << "[mm]" << std::endl;
   close(fd);
   return distance;
}
