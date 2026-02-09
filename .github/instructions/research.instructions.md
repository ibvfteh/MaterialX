---
applyTo: '**'
---

# Overview

Insert overview text here. The agent will only see this should they choose to apply the rule.


1. it's a research project. Don't assume anything that can impact the final evaluation results. Always ask user when you feel not confident based on incomplete user's input
2. Don't overprocess edge cases. It's a research code. Keep it simple to maintain. Better to code it simple and throw exceptions and print errors when you're not confident about the data, inputs, shape sizes, rathe then support all possible input configurations
3. DOn't create tests if you were asked to do it explicitly. But code the classes in a way they could be tested
4. Separate resposibilities between clases, introduce new classes if it's required. Combine new data fields and logicts into one new class if it minimizes code dubplicatons
5. Don't use "from * import *" or "import *" inside functions or methods. put them in the beginning of files