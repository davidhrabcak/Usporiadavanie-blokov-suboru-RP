from typing import List, Optional
from validation.base_validator import BaseValidator

class Backtrack:
    def __init__(self, validator: BaseValidator, all_chunks: List[str]):
        self.validator = validator
        self.all_chunks = all_chunks
    
    def reconstruct_all(self, output_file: str) -> List[str]:
        results = []
        with open(output_file, "a") as file:
            for i, chunk in enumerate(self.all_chunks):
                print(f"Trying starting chunk {i+1}/{len(self.all_chunks)}: {repr(chunk)}")
                remaining = self.all_chunks[:i] + self.all_chunks[i+1:]
                self._backtrack(chunk, remaining, file, results, find_all=True)
        return results
    
    def reconstruct_one(self, output_file: str) -> Optional[str]:
        with open(output_file, "a") as file:
            for i, chunk in enumerate(self.all_chunks):
                print(f"Trying starting chunk {i+1}/{len(self.all_chunks)}: {repr(chunk)}")
                remaining = self.all_chunks[:i] + self.all_chunks[i+1:]
                results = []
                self._backtrack(chunk, remaining, file, results, find_all=False)
                if results:
                    return results[0]
        return None
    
    def _backtrack(self, current_text: str, remaining: List[str], file, 
                   results: List[str], find_all: bool):
        if not remaining:
            if self.validator.validate_text(current_text, self.all_chunks):
                print("Found valid reconstruction!\a")
                results.append(current_text)
                file.write(f"{current_text}\n")
            return
        
        if not find_all and results:
            return
        
        for i, chunk in enumerate(remaining):
            candidate = current_text + chunk
            if self.validator.validate_chunk(candidate):
                next_remaining = remaining[:i] + remaining[i+1:]
                self._backtrack(candidate, next_remaining, file, results, find_all)