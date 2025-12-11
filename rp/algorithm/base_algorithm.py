from abc import ABC, abstractmethod


class BaseAlgorithm(ABC):

    @abstractmethod
    def reconstruct_all(self, output_file: str):
        pass

    @abstractmethod
    def reconstruct_one(self, output_file: str):
        pass