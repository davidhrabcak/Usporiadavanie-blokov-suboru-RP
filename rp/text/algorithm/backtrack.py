"""used for typing of method arguments"""
from typing import List, Optional
from validation.base_validator import BaseValidator
from algorithm.base_algorithm import BaseAlgorithm

class Backtrack(BaseAlgorithm):
    """Performs backtracking algorithm"""
    def __init__(self, validator: BaseValidator, all_chunks: List[str]):
        self.validator = validator
        self.all_chunks = all_chunks

    def reconstruct_all(self, output_file: str) -> List[str]:
        """Reconstructs all valid text based on validator"""
        l = len(self.all_chunks)
        results: List[str] = []
        used = [False] * l

        with open(output_file, "a") as f:
            for i in range(l):
                print(f"Trying starting chunk {i+1}/{l}: {repr(self.all_chunks[i])}")
                used[i] = True
                self._backtrack(self.all_chunks[i], used, f, results, find_all=True)
                used[i] = False

        return results

    def reconstruct_one(self, output_file: str) -> Optional[str]:
        """Reconstructs first valid text based on validator"""
        used = [False] * len(self.all_chunks)

        with open(output_file, "a") as f:
            for i in range(len(self.all_chunks)):
                used[i] = True
                print(f"Trying starting chunk {i+1}/{len(self.all_chunks)}: {repr(self.all_chunks[i])}")
                result = self._backtrack(self.all_chunks[i], used, f, [], find_all=False)
                used[i] = False
                if result: return result

        return None


    def _backtrack(self, current_text: str, used: List[bool], file, results: List[str], find_all: bool) -> List[str] | Optional[str]:
        """helper backtracking function"""
        if all(used):
            if self.validator.validate_text(current_text, self.all_chunks):
                results.append(current_text)
                file.write(current_text + "\n")
                return current_text if not find_all else None
            return None
        for i in range(len(self.all_chunks)):
            if not used[i]:
                ch = self.all_chunks[i]
                if self.validator.validate_chunk(current_text, ch):
                    used[i] = True
                    found = self._backtrack(current_text + ch, used, file, results, find_all)
                    used[i] = False
                    if found and not find_all: return found

        return None
