from dictionary.standard_dictionary import StandardDictionary
from validation.standard_validator import StandardValidator
from segment import Segmenter
from algorithm.backtrack import Backtrack

def main():
    seg = Segmenter("in.txt", "chunk_file.txt", 32)
    seg.segment()

    dictionary = StandardDictionary()
    dictionary.load("dict_en.txt")
    validator = StandardValidator(dictionary)
    reconstructor = Backtrack(validator, seg.get_chunks())

    print(reconstructor.reconstruct_all("found.txt"))
    print(reconstructor.reconstruct_one("found.txt"))


if __name__ == "__main__":
    main()