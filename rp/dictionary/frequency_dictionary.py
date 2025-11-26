from .base_dictionary import BaseDictionary
from typing import *

class FrequencyDictionary(BaseDictionary):
    def load(self, source: str) -> None:
        with open(source, 'r') as f:
            for line in f:
                words = line.split()
                if len(words) >= 2:
                    self.data[words[0]] = words[1:]

    def get_following_words(self, first_word: str) -> List[str]:
        return self.data.get(first_word, [])