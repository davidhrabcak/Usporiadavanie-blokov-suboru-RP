from validation.base_validator import BaseValidator
from dictionary.two_sequence_dictionary import FrequencyDictionary
from typing import List

class FrequencyValidator(BaseValidator):
    def __init__(self, dictionary: FrequencyDictionary, chunks: List[str]):
        self.dictionary = dictionary
        self.all_chunks = chunks
    
    def validate_chunk(self, chunk1: str, chunk2: str) -> bool:
        ch1 = chunk1.split()
        ch2 = chunk2.split()

        if not ch1 or not ch2: return True
        if len(ch1) < 2 and len(ch2) < 2: return True


        if chunk1[-1] == ' ' or chunk2[0] == ' ':
            return self.dictionary.contains(ch1[-1], ch2[0])

        else:
            new_word = ch1[-1] + ch2[0]
            if new_word not in self.dictionary.data: return False
            if len(ch1) > 2:
                if not self.dictionary.contains(ch1[-2], new_word): return False
            if len(ch2) > 2:
                if not self.dictionary.contains(new_word, ch2[1]): return False
            return True
    
    def validate_text(self, text: str, required_chunks:str) -> bool:
        for chunk in required_chunks:
            if chunk not in text: return False
        return True