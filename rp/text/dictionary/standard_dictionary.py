"""used for typing of method arguments"""

from typing import List
import re
from dictionary.base_dictionary import BaseDictionary

class StandardDictionary(BaseDictionary):
    """Creates dictionary in 2 possible ways:\n
       -loading from a file\n
       -creating it from complete words inside chunks"""

    def load(self, source: str) -> None:
        """Loads dictionary from source file"""
        with open(source, 'r') as f:
            for line in f:
                word = line.strip().lower()
                if word:
                    self.data[word] = True

    def load_from_chunks(self, chunks: List[str]) -> None:
        """Creates a dictionary from words inside chunks"""

        for chunk in chunks:
            words = chunk.split()
            if len(words) > 2:
                word = words[1:-1]

            for word in words:
                word = re.sub(r"[^A-Za-z\-]", '', word)
                self.data[word.lower()] = True
