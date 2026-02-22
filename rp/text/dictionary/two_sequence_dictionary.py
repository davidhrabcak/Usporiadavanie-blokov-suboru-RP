"""used for typing of method arguments"""

from typing import List
from dictionary.base_dictionary import BaseDictionary

class FrequencyDictionary(BaseDictionary):
    """Dictionary that holds words and 10 most frequent subsequent words"""

    def load(self, source: str) -> None:
        """Loads created dictionary from a file"""
        with open(source, 'r') as f:
            for line in f:
                words = line.split()
                if len(words) >= 2:
                    self.data[words[0]] = words[1:]

    def get_following_words(self, first_word: str) -> List[str]:
        """Returns list associated with first_word"""
        return self.data.get(first_word, [])
