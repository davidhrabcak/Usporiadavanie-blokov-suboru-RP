// currently not used in the implementation - has issues that need to be fixed

#include <iostream>
#include <string>
#include <syscall.h>
#include <string.h>
using namespace std;
// compile to binary named segment


// 1st argument: input file
// 2nd argument: output file
// 3rd argument: segment size
int main(int argc, char const *argv[])
{   //TODO fix joining of files and remove after saving segments
    string command = "split -b ";
    string part1 = " -d ";
    string part2 = " segment_";
    string command2 = "ls segment_* | shuf | xargs cat | paste -sd '\n' > ";

    command.append(argv[3]);
    command.append(part1);
    command.append(argv[1]);
    command.append(part2);

    command2.append(argv[2]);

    system(command.c_str());
    cout << command2 << endl;
    system(command2.c_str());
    system("rm segment_*");
    return 0;
}

//split -b 3 -d bigfile.txt segment_
//cat segment_* | paste -sd '\n' > chunks_with_newlines.txt