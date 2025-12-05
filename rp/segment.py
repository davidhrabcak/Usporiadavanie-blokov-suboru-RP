"""used for calling commands and typing of method calls"""
import subprocess
from typing import List

class Segmenter():
    """Handles segmentation of the original file and loading segment chunks"""
    def __init__(self, input_file, output_file, segment_size):
        self.input_file = input_file
        self.output_file = output_file
        self.segment_size = segment_size

    def segment(self) -> None:
        """Segments input file into chunks of segment_size and saves them into the output file"""
        subprocess.call("split -b" + str(self.segment_size) 
                        + " -d " + str(self.input_file) + " segment_", shell=True)
        subprocess.call("ls segment_* | shuf | xargs -I {} sh -c 'cat {}; echo' >"
                        + str(self.output_file), shell=True)
        subprocess.call("rm segment_*", shell=True)

    def get_chunks(self) -> List[str]:
        """Return chunks in output file"""
        result = []

        with open(self.output_file, encoding="ascii") as f:
            for line in f:
                if len(line) > 1:
                    result.append(line[:-1])
                else:
                    result.append(line)
        return result

#def main():
#    seg = Segmenter("in.txt", "chunk_file.txt", 16)
#    seg.segment()
#    print(seg.get_chunks())

#if __name__ == "__main__":
#    main()
