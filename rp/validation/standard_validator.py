from .base_validator import BaseValidator
from dictionary.standard_dictionary import StandardDictionary
from typing import List

class StandardValidator(BaseValidator):
    def __init__(self, dictionary: StandardDictionary):
        self.dictionary = dictionary
    
    def validate_chunk(self, chunk: str) -> bool:
        words = chunk.split()
        if len(words) > 2:
            words = words[1:-1]
        
        return all(self.dictionary.contains(w) for w in words if w.isalpha())
    
    def validate_text(self, text: str, required_chunks: List[str]) -> bool:
        for chunk in required_chunks:
            if chunk not in text:
                return False
        
        words = text.split()
        return all(self.dictionary.contains(w) for w in words if w.isalpha())