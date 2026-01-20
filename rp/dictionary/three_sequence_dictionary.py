"""used for typing method arguments"""
from typing import List, Dict
import re

class ThreeSequenceDictionary:
    """Dictionary that saves three word sequences instead of only 2"""
    def __init__(self):
        super().__init__()
        self.dictionary: List[str] = []
        self.coordinates: Dict[str, List[int]] = {}

    def load(self, source_file: str, coordinates_file: str) -> None:
        """loads the dictionary from source files"""
        with open(source_file, "r", encoding="ascii") as f1:
            for line in f1:
                for word in line.split():
                    word = re.sub(r"[^A-Za-z\-]", '', word)
                    self.dictionary.append(word.lower())

        with open(coordinates_file, "r", encoding="ascii") as f2:
            for line in f2:
                words = line.split()
                self.coordinates[words[0]] = list(map(int, words[1:]))

    def contains(self, middle_word: str, first_last: List[str]) -> bool:
        """returns if there is an entry with first_word where next_words are the following words in sequence"""
        middle_word = re.sub(r"[^A-Za-z\-]", '', middle_word).lower()
        first_last = list(map(lambda x: re.sub(r"[^A-Za-z\-]", '', x).lower(), first_last))
        if middle_word not in self.coordinates: return False

        coordinates = self.coordinates.get(middle_word, [])
        for coord in coordinates:
            if 0 < coord < len(self.dictionary) - 1:
                if (self.dictionary[coord - 1] == first_last[0] and
                        self.dictionary[coord + 1] == first_last[1]):
                    return True
        return False
