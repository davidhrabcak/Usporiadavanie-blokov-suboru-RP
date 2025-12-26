from typing import List
from dictionary.three_sequence_dictionary import ThreeSequenceDictionary

class FrequencyThreeWordsValidator:
    def __init__(self, dictionary: ThreeSequenceDictionary, chunks: List[str]):
        self.dictionary = dictionary
        self.all_chunks = chunks

    def validate_chunk(self, chunk1: str, chunk2: str) -> bool:
        words = (chunk1 + chunk2).split()
        if len(words) < 3: return True
        if (len(words) == 3 and words[0] in self.dictionary.dictionary):
            return self.dictionary.contains(words[0], words[1:])
        sequence = (chunk1[-2] + chunk1[-1] + chunk2[0] + chunk2[1]).split()
        return self.dictionary.contains(sequence[0], sequence[1:])

    def _validate_sequence(self, words: List[str]) -> bool:
        if len(words) < 3: return True
        return self.dictionary.contains(words[0], words[1:])

    def validate_text(self, text: str, required_chunks: List[str]) -> bool:
        if not self._validate_sequence(text.split()): return False
        return all(ch in text for ch in required_chunks)