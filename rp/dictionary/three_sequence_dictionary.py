"""used for typing method arguments"""
from typing import List, Dict

class ThreeSequenceDictionary():
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
                    self.dictionary.append(word)

        with open(coordinates_file, "r", encoding="ascii") as f2:
            for line in f2:
                words = line.split()
                self.coordinates[words[0]] = list(map(int, words[1:]))

    def contains(self, first_word: str, next_words: List[str]) -> bool:
        """returns if there is an entry with first_word where next_words are the following words in sequence"""
        coordinates = self.coordinates[first_word]
        next_words = next_words[:2]
        for c in coordinates:
            i = 1
            for w in next_words:
                if w != self.dictionary[(c + i)]:
                    i += 1
                    continue
                else: i += 1
            return True
        return False
