"""used for typing of method arguments
and accessing arguments the script has been called with"""
from sys import argv
from typing import Dict, List
import re


def main():
    """main of three segment file creator"""
    with open(argv[1], "r") as f:
        input_file = f.read()
    with open(argv[2], "a") as coordinates:
        i = 0
        words = input_file.split()
        dictionary: Dict[str, List[int]] = {}
        for word in words:
            word =  re.sub(r"[^A-Za-z\-]", '', word)
            dictionary.setdefault(word.lower(), []).append(i)
            i += 1

        for key, value in dictionary.items():
            coordinates.write(key.lower())
            for v in value:
                coordinates.write(f" {v} ")
            coordinates.write("\n")

if __name__ == "__main__":
    main()
