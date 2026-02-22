from dictionary.standard_dictionary import StandardDictionary
from validation.standard_validator import StandardValidator

def test_validate_chunk():
    std = StandardDictionary()
    std.load("dict_en.txt")
    assert "first" in std.data

    validator = StandardValidator(std)
    assert validator.validate_chunk("This is the fir", "st sentence.")

    assert not (validator.validate_chunk("a pareng", "fdgj l"))

    assert validator.validate_chunk("first part ", " second part")
    assert validator.validate_chunk("first part ", "second part")
    assert validator.validate_chunk("first part", " second part")
    assert not validator.validate_chunk("first part", "second part")

def test_validate_text():
    std = StandardDictionary()
    std.load("dict_en.txt")

    required_chunks = ["This ", " is", " the ", "second", " sentence."]
    assert StandardValidator(std).validate_text("".join(required_chunks), required_chunks)
    assert StandardValidator(std).validate_text("This is the second sentence.", required_chunks)
    assert StandardValidator(std).validate_text("This the is sentence. second", required_chunks)
    assert not StandardValidator(std).validate_text("This 3is the !#$sentence. second_ )!+", required_chunks)
    assert not StandardValidator(std).validate_text("This is the sentence.", required_chunks)

    required_chunks = ["T", "his ", "is", " in", " the", " in", " correct ", "ord", "er"]
    assert StandardValidator(std).validate_text("".join(required_chunks), required_chunks)
    assert not StandardValidator(std).validate_text(" ".join(required_chunks[:-1]), required_chunks)
    required_chunks.sort()
    assert not StandardValidator(std).validate_text("".join(required_chunks), required_chunks)
def main():
    test_validate_chunk()
    test_validate_text()

if __name__ == "__main__":
    main()
