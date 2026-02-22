"""used for typing of method arguments
and accessing arguments the script has been called with"""
from typing import Dict, List
import re

class ThreeSequenceDictionaryCreator:
    def __init__(self, input_file: str, coordinates_file: str):
        with open(input_file, "r") as ifile:
            self.input_file = ifile.read()
        self.coordinates = coordinates_file
            
    
    def run(self):
        with open(self.coordinates, "a") as coordinates:
            i = 0
            words = self.input_file.split()
            dictionary: Dict[str, List[int]] = {}
            for word in words:
                word =  re.sub(r"[^A-Za-z\-]", '', word)
                dictionary.setdefault(word.lower(), []).append(i)
                i += 1

            for key, value in dictionary.items():
                coordinates.write(key.lower())
                for v in value:
                    coordinates.write(f" {v} ")
                coordinates.write("\n")
