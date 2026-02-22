from abc import ABC, abstractmethod
from typing import List

class BaseValidator(ABC):
    @abstractmethod
    def validate_chunk(self, chunk1: str, chunk2: str) -> bool:
        pass
    
    @abstractmethod
    def validate_text(self, text: str, required_chunks: List[str]) -> bool:
        pass