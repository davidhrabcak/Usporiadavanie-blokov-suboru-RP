#include <iostream>
#include <string>
#include <syscall.h>
using namespace std;

// 1st argument: input file
// 2nd argument: output file
// 3rd argument: segment size
int main(int argc, char const *argv[]) {

    // split -b [arg_3] -d [arg_1] segment_
    // ls segment_* | shuf | xargs -I {} sh -c 'cat {}; echo' > [arg_2]
    string command = "split -b ";
    string command2 = "ls segment_* | shuf | xargs -I {} sh -c 'cat {}; echo' >";

    command.append(argv[3]).append(" -d ").append(argv[1]).append(" segment_");
    command2.append(argv[2]);

    system(command.c_str());
    system(command2.c_str());
    system("rm segment_*");
    return 0;
}