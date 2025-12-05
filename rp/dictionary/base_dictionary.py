"""provides abstract structures"""
from abc import ABC, abstractmethod

class BaseDictionary(ABC):
    """Base structure that every dictionary implements"""
    def __init__(self):
        self.data = {}

    @abstractmethod
    def load(self, source: str) -> None:
        """Loads frequency dictionary from file"""

    def contains(self, first_word: str, second_word: str) -> bool:
        """Returns if the first word list contains the second word"""
        if first_word in self.data:
            return second_word in self.data[first_word]
        return False
