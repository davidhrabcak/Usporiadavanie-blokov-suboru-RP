from .base_validator import BaseValidator
from dictionary.frequency_dictionary import FrequencyDictionary
from typing import List

class FrequencyValidator(BaseValidator):
    def __init__(self, dictionary: FrequencyDictionary, chunks: List[str]):
        self.dictionary = dictionary
        self.all_chunks = chunks
    
    def validate_chunk(self, chunk1: str, chunk2: str) -> bool:
        words = (chunk1 + chunk2).split()
        if len(words) <= 1:
            return True
        
        for i in range(len(words) - 1):
            current_word = words[i]
            next_word = words[i + 1]
            
            if current_word in self.dictionary.data:
                if next_word not in self.dictionary.data[current_word]:
                    return False
        return True
    
    def validate_text(self, text: str, required_chunks: List[str]) -> bool:
        #TODO may want to implement, although it won't be used anywhere
        return True