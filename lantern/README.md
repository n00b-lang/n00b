# LANTERN: _Lan_guage  _T_oolkit... _er_, _n_00b's

The goal here is to make it really easy to make your own programming
language if you're willing to deal with common semantics for things.

You basically have to just specify a grammer in EBNF, add simple
annotations to your productions to help define the semantics. You can
denote blocks, loops, functions, types, etc.

Without writing real code, you can get a tokenizer, parser (emitting
full parse trees for any context free language, and a simple IR that
can round-trip to a textual language. You can also get full type
checking and error checking, and implement type inferencing w/o real
code.

There are ~40 annotation types at this point, including several to
make it easy to do a modern pretty printer.

## Organization

- A few modern grammars written in EBNF, with some annotations. These
  may not be a proper superset of real functionality; I haven't
  bothered trying to keep things up to date.

- On the practical side, that includes json and CEL (google's Common
  expresison language). But I've tested on any major language grammar
  I can find, so we also have decent parsers for Go, C#, Java,
  Javascript, Kotlin, Lua, Php, Python, Ruby, Rust, Swift and
  Typescript. Though, don't expect this project to keep up w/ fast
  moving grammars.
  
- The c_ncc grammar is from my ncc project, which uses this BNF
  project to parse C *and* a bunch of my extensions. As of this
  writing, it definitely handles a huge swath of headers across Linux,
  MacOS and Windows.

- I also am very wary about adding AI into a project at this point,
  but it's become necessary to move at a modern pace. I'm trying to
  move tiny pieces at a time via AI, but may lead to some weird side
  effects until I've fully cleaned up.

- The type hashing algorithm is written to be general across most
  modern programming languages; in most languages, you should be able
  to encode type equivelence rules such that, if `A and `B are the
  same type, they will give the same hash value. And if they are NOT
  the same type, they will give different values.

  This can be made to work with structural or named equivolence, or
  even a mixture of the two.

## Background

This is a partial extraction of a bunch of language tools I've been
honing for more than a decade.

I have *not* yet ported in codegen backends or real object file
support. I will get to it sooner than later.

I also have not moved in the entire constraint system; it is pretty
much vanilla unification right now.

The previous iteration of this code was written in `ncc`, my C
extensions. This returns a lot of the capability to C, though

I did the extraction because I am considering using it as a basis to
hand-writea bootstrap n00b compiler.

I did allow coding agents to "help" in the move, but despite being
well spec'd, I am not particularly happy with the liberties taken.

I'm probably going to avoid major API changes though until I manually
redo most of this once the next version of n00b is up.


## Note well

- As mentioned above, a LOT of missing here, including my unicode
  implementation and constraint library. Don't expect stability yet!