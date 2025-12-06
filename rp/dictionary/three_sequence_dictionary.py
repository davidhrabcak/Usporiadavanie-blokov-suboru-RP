"""used for typing method arguments"""
from typing import List, Optional, Dict
from dictionary.base_dictionary import BaseDictionary


#TODO nedomyslene ako bude fungovat contains - ako budeme vediet z coordinates,
# ktore v poradi je slovo v trojici?
class ThreeSequenceDictionary(BaseDictionary):
    """Dictionary that saves three word sequences instead of only 2"""
    def __init__(self):
        super().__init__()
        self.sequences: List[str] = []
        self.coordinates: Dict[int, List[str]] = {}

    def load(self, sequences_file: str, coordinates_file: str) -> None:
        """loads the dictionary from source files"""
        self.sequences = []
        with open(sequences_file, "r", encoding="ascii") as f1:
            for line in f1:
                for w in line.split():
                    self.sequences.append(w)
        with open(coordinates_file, "r", encoding="ascii") as f2:
            for line in f2:
                words = line.split()
                self.coordinates[words[0]] = list(map(int, words[1:]))

    def contains(self, first_word: Optional[str],
                 second_word: Optional[str], third_word: Optional[str]) -> bool:
        """returns if there is an entry with first_word"""
        if first_word:
            if second_word:
                if third_word:
        elif second_word:
            if third_word:
        elif third_word: