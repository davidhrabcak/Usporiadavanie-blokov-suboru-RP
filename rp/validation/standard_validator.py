from validation.base_validator import BaseValidator
from dictionary.standard_dictionary import StandardDictionary
from typing import List
import re

class StandardValidator(BaseValidator):
    def __init__(self, dictionary: StandardDictionary):
        self.dictionary = dictionary
    
    def validate_chunk(self, chunk1: str, chunk2: str) -> bool:
        if chunk1[-1] == ' ' or chunk2[0] == ' ':
            return True
        else:
            if len(chunk1.split()) < 2 or len(chunk2.split()) < 2:
                return True
            word = chunk1.split()[-1] + chunk2.split()[0]
            return word.lower() in self.dictionary.data
    
    def validate_text(self, text: str, required_chunks: List[str]) -> bool:
        for chunk in required_chunks:
            if chunk not in text:
                return False
        
        words = text.split()
        for w in words:
            w = re.sub(r"[^A-Za-z\-]", '', w)
            if w.lower() not in self.dictionary.data:
                return False
        return True