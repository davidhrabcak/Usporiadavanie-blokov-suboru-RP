from .base_dictionary import BaseDictionary
from typing import *

class StandardDictionary(BaseDictionary):
    def load(self, source: str) -> None:
        with open(source, 'r') as f:
            for line in f:
                word = line.strip().lower()
                if word:
                    self.data[word] = True

    def load_from_chunks(self, chunks: List[str]) -> None:
        for chunk in chunks:
            words = chunks.split()
            if len(words) > 2:
                word = words[1:-1]

            for word in words:
                if word.isalpha(): self.data[word.lower()] = True
