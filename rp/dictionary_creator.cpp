#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <unordered_map>
using namespace std;

vector<string> split_text(string sen) {
    stringstream ss(sen);
    string word;
    vector<string> words;
    
    while (ss >> word) {
        words.push_back(word);
    }
    
    return words;
}
//TODO markov chain frequency analysis - use unordered_map<string, vector<string>>,
// where first element in vector is the most frequent successor
int main(int argc, char const *argv[]) {
    vector<string> sources;
    ifstream file("input_books.txt"); // input filename
    stringstream buffer;
    buffer << file.rdbuf();
    string data = buffer.str();
    vector<string> words = split_text(data);

    unordered_map<string, vector<string>> dictionary;
    for (int i = 0; i < words.size() - 1; i++) {
    }
}
