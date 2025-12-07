"""used for typing of method arguments
and accessing arguments the script has been called with"""
from typing import List
from sys import argv

def main():
    """main of three segment file creator"""
    with open(argv[1], "r", encoding="ascii") as f:
        input_file = f.read()
    with open(argv[2], "a", encoding="ascii") as sequences:
        with open(argv[3], "a", encoding="ascii") as coordinates:
          #TODO mask that isolates 3 words, puts their coordinates into coordinates
          # and saves tuple into sequences
            pass
if __name__ == "__main__":
    main()
