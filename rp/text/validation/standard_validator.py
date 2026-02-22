from validation.base_validator import BaseValidator
from dictionary.standard_dictionary import StandardDictionary
from typing import List
import re

_WORD_RE = re.compile(r"[^A-Za-z\-]")

class StandardValidator(BaseValidator):
    def __init__(self, dictionary: StandardDictionary):
        self.dictionary = dictionary

    def validate_chunk(self, chunk1: str, chunk2: str) -> bool:
        if chunk1[-1] == ' ' or chunk2[0] == ' ':
            return True

        words1 = chunk1.split()
        words2 = chunk2.split()

        if len(words1) < 2 or len(words2) < 2:
            return True

        combined = (words1[-1] + words2[0]).lower()
        return combined in self.dictionary.data

    def validate_text(self, text: str, required_chunks: List[str]) -> bool:
        for chunk in required_chunks:
            if chunk not in text:
                return False

        for raw in text.split():
            word = _WORD_RE.sub('', raw).lower()
            if word and word not in self.dictionary.data:
                return False

        return True