from abc import ABC, abstractmethod

class BaseDictionary(ABC):
    def __init__(self):
        self.data = {}

    @abstractmethod
    def load(self, source: str) -> None:
        pass

    def contains(self, word: str) -> bool:
        return word.lower() in self.data