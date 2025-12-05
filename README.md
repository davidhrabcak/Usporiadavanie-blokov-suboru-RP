## File Carving
**Overview**
This project takes a text that has been segmented into multiple chunks and attempts to reconstruct the original text by finding valid permutations of these chunks. It validates reconstructions different methods.

**Features**
 1. Standard dictionary validation (check if words exist)
 2. Frequency-based validation (checks word sequences probability)
 3. Custom dictionary importing
 4. Frequency-dictionary creator from your own data

**How to use**

Every choice is made by commenting or uncommenting neccessary lines in main.py.

 1. If needed, segment an input file
 2. Choose method of validation and dictionary
 3. Choose reconstruction strategy (find first/find all valid)
 4. Run main - reconstructed texts will be saved to found.txt

**Progress**
 - [ ] Dictionary creator for 3-word sequences
 - [ ] Text reconstruction algorithm that uses frequency the frequency dictionary
 - [X] Clean up file structure
 - [X] Rewrite everything to python
