from typing import List
from dictionary.three_sequence_dictionary import ThreeSequenceDictionary

class FrequencyThreeWordsValidator:
    def __init__(self, dictionary: ThreeSequenceDictionary, chunks: List[str]):
        self.dictionary = dictionary
        self.all_chunks = chunks

    def validate_chunk(self, chunk1: str, chunk2: str) -> bool:
        if len(chunk1.split()) < 2 or len(chunk2.split()) < 2: return True

        if chunk1[-1] == ' ' or chunk2[0] == ' ':
            words = [chunk1.split()[-2], chunk1.split()[-1], chunk2.split()[0], chunk2.split()[1]]
            return (self.dictionary.contains(words[2], [words[1], words[3]]) and
                    self.dictionary.contains(words[1], [words[0], words[2]]))
        else:
            words = [chunk1.split()[-2], chunk1.split()[-1] + chunk2.split()[0], chunk2.split()[1]]
            return self.dictionary.contains(words[1], [words[0], words[2]])

    def validate_text(self, text: str, required_chunks: List[str]) -> bool:
        return all(ch in text for ch in required_chunks)