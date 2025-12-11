from typing import List
from dictionary.three_sequence_dictionary import ThreeSequenceDictionary
from validation.base_validator import BaseValidator

class FrequencyThreeWordsValidator(BaseValidator):
    def __init__(self, dictionary: ThreeSequenceDictionary, chunks: List[str]):
        self.dictionary = dictionary
        self.all_chunks = chunks

    def validate_chunk(self, chunk: str) -> bool:
        words = chunk.split()[1:-1]
        return self._validate_sequence(words)

    def _validate_sequence(self, words: List[str]) -> bool:
        if len(words) < 3: return True
        for word1, word2, word3 in zip(words, words[1:], words[2:]):
            found = False
            for coord in self.dictionary.coordinates[word1]:
                if coord % 3 == 0:
                    if not (word2.lower() in self.dictionary.sequences[coord // 3][1]
                            and word3.lower() in self.dictionary.sequences[coord][2]):
                        return False
                    found = True
                if coord % 3 == 1:
                    if not word3.lower() in self.dictionary.sequences[(coord - 1) // 3][2]:
                        return False
                    found = True
            if not found: return False
        return True

    def validate_text(self, text: str, required_chunks: List[str]) -> bool:
        if not self._validate_sequence(text.split()): return False
        return all(ch in text for ch in required_chunks)