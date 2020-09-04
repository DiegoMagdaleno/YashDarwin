# brace-y.tst: yash-specific test of brace expansion

setup -d

(
setup 'set -o braceexpand'

test_oE 'simple brace expansion'
bracket {1,22,333}
bracket a{1,2}b
__IN__
[1][22][333]
[a1b][a2b]
__OUT__

test_oE 'joined brace expansions'
bracket {aa,b}{1,22,333}
bracket a{1,2}b{3,4}c
__IN__
[aa1][aa22][aa333][b1][b22][b333]
[a1b3c][a1b4c][a2b3c][a2b4c]
__OUT__

test_oE 'empty elements'
bracket a{1,,2}b
bracket x{,,}
bracket {,,}
__IN__
[a1b][ab][a2b]
[x][x][x]
[][][]
__OUT__

test_oE 'nested brace expansions'
bracket a{p,q{1,2}r}b
__IN__
[apb][aq1rb][aq2rb]
__OUT__

test_oE 'quoted elements'
bracket \a{\1,"2",'3'}''
bracket a{\{,\,,\}}
bracket a{\\,\',\"}
__IN__
[a1][a2][a3]
[a{][a,][a}]
[a\][a'][a"]
__OUT__
#'

test_oE 'simple sequences'
bracket {1..3}
bracket a{1..3}b
bracket {1..1}
bracket {-3..2}
__IN__
[1][2][3]
[a1b][a2b][a3b]
[1]
[-3][-2][-1][0][1][2]
__OUT__

test_oE 'multi-digit sequences'
bracket {99..101}
bracket {099..101}
bracket {-101..-99}
bracket {-101..-099}
__IN__
[99][100][101]
[099][100][101]
[-101][-100][-99]
[-101][-100][-099]
__OUT__

test_oE 'sequences with non-default steps'
bracket {2..11..3}x
bracket {2..11..-3}y
bracket {0..5..2}
__IN__
[2x][5x][8x][11x]
[2y]
[0][2][4]
__OUT__

test_oE 'descending sequences'
bracket {2..-2}x
bracket {-2..-11..-3}y
bracket {-2..-11..3}z
bracket {0..-5..-2}
__IN__
[2x][1x][0x][-1x][-2x]
[-2y][-5y][-8y][-11y]
[-2z]
[0][-2][-4]
__OUT__

test_oE 'invalid step'
bracket {1..5..0} {0..0..0}
__IN__
[{1..5..0}][{0..0..0}]
__OUT__

test_oE 'not brace expansions'
bracket { } {a,b a,b}
bracket a{b}c a\{b,c}d a{b\,c}d a{b,c\}d a'{'b,c}d a{b,c"}"d
bracket a{1..}b a{..1}b {1.9}
bracket \{1..2} {\1..2} {1\..2} {1..\2} {1..2\}
__IN__
[{][}][{a,b][a,b}]
[a{b}c][a{b,c}d][a{b,c}d][a{b,c}d][a{b,c}d][a{b,c}d]
[a{1..}b][a{..1}b][{1.9}]
[{1..2}][{1..2}][{1..2}][{1..2}][{1..2}]
__OUT__

test_oE 'result of tilde expansion is not subject to brace expansion'
HOME=/{1,2}
bracket ~
__IN__
[/{1,2}]
__OUT__

test_oE 'result of parameter expansion is not subject to brace expansion'
a='{1,2}'
bracket $a
__IN__
[{1,2}]
__OUT__

)

test_oE 'disabled brace expansion'
bracket {{aa,b}{1,22,333}}{1..9}
__IN__
[{{aa,b}{1,22,333}}{1..9}]
__OUT__

# vim: set ft=sh ts=8 sts=4 sw=4 noet:
