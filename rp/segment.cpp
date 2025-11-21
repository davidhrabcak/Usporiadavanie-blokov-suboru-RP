#include <iostream>
#include <string>
#include <syscall.h>
#include <string.h>
using namespace std;

// 1st argument: input file
// 2nd argument: output file
// 3rd argument: segment size
int main(int argc, char const *argv[])
{   
    string command = "split -b ";
    string part1 = " -d ";
    string part2 = " segment_";
    string command2 = "cat segment_* | paste -sd '\\n' > ";
    command.append(argv[3]);
    command.append(part1);
    command.append(argv[1]);
    command.append(part2);

    command2.append(argv[2]);

    system(command.c_str());
    system(command2.c_str());
    return 0;
}

//split -b 3 -d bigfile.txt segment_
//cat segment_* | paste -sd '\n' > chunks_with_newlines.txt