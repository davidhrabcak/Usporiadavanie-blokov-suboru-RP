"""used for typing of method arguments
and accessing arguments the script has been called with"""
from sys import argv
from typing import Dict, List


def main():
    """main of three segment file creator"""
    with open(argv[1], "r", encoding="ascii") as f:
        input_file = f.read()
    with open(argv[2], "a", encoding="ascii") as sequences:
        with open(argv[3], "a", encoding="ascii") as coordinates:
            i = 0
            words = input_file.split()
            dictionary: Dict[str, List[int]] = {}
            for word1, word2, word3 in zip (words[i:], words[i+1:], words[i+2:]):
                dictionary.setdefault(word1.lower(), []).append(i)
                dictionary.setdefault(word2.lower(), []).append(i+1)
                dictionary.setdefault(word3.lower(), []).append(i+2)
                sequences.write(f"{word1.lower()} {word2.lower()} {word3.lower()}\n")
                i += 1

            for key, value in dictionary.items():
                coordinates.write(key.lower())
                for v in value:
                    coordinates.write(f" {v} ")
                coordinates.write("\n")

if __name__ == "__main__":
    main()
