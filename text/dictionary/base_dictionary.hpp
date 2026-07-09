#ifndef RP_BASE_DICTIONARY_HPP
#define RP_BASE_DICTIONARY_HPP

#include <string>

/** Base structure that every dictionary implements */
class BaseDictionary {
public:
    virtual ~BaseDictionary() = default;

    /** Loads a dictionary from the given source. Subclasses vary in how
     *  many source files they require. */
    virtual void load(const std::string& source) = 0;
};

#endif
