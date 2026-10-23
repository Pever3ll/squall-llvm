# Squall Instruction Set

### ALU Instructions

#### Monadic Instruction

``` regex
<instr> (\(<idest>\)|<idest>)(\.(l|r)) (l|r)? <imm>
```

Example: \
`add (0x03).l r 0x50` creates an add instruction where `v` is added to 50 and creates a token with the result as the left operand of the third instruction after this. The carry value is sent to the next instruction as the right operand.

##### Instruction Options

#### Dyadic Instruction

``` regex
<instr>(\.(r|c))? (\(<idest>\)|<label>)\.(l|r) (l|r)? (\[<fdest>\]|\(<fdest>\)|<fdest>)
```

### Non ALU

#### Extract

``` regex
extr(\.(r|c))? (\(<idest>\)|<label>)\.(l|r) (\(<ipayload>\)|<label>)\.(l|r) (\[<fdest>\]|\(<fdest>\)|<fdest>)
```

#### Send

``` regex
send(\.(r|c))? (\(<idest>\)|<label>)\.(l|r) (\[<fdest>\]|\(<fdest>\)|<fdest>)
```

#### Write

``` regex
write <ptr>
```
