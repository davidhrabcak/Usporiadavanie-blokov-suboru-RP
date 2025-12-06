"""used for typing method arguments"""
from typing import List, Dict

class ThreeSequenceDictionary():
    """Dictionary that saves three word sequences instead of only 2"""
    def __init__(self):
        super().__init__()
        self.sequences: List[(str, str, str)] = []
        self.coordinates: Dict[int, List[str]] = {}

    def load(self, sequences_file: str, coordinates_file: str) -> None:
        """loads the dictionary from source files"""
        self.sequences = []
        with open(sequences_file, "r", encoding="ascii") as f1:
            for line in f1:
                w = line.split()
                self.sequences.append((w[0], w[1], w[2]))

        with open(coordinates_file, "r", encoding="ascii") as f2:
            for line in f2:
                words = line.split()
                self.coordinates[words[0]] = list(map(int, words[1:]))

    def contains(self, first_word: str, next_words: List[str]) -> bool:
        """returns if there is an entry with first_word"""
        coordinates = self.coordinates[first_word]
        for c in coordinates:
            sequence = self.sequences[c]

            for w in next_words:
                if w not in sequence:
                    continue
                return True
        return False
