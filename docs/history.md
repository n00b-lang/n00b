# History of n00b

I stumbled into programming through MUDs, which married my love of writing with my love of games... interactive fiction like Zork. But instead of writing any games, I quickly fell into building the platform to build the games I wanted, and got involved early in the design of the underlying language in which the games were built.

I was in college at the time, and was loving programming, so decided to see if I could do computer science. Early on, I took a class, Usability Engineering. The first lecure started with the professor, Randy Pausch, ranting about how most technologists don't do a good job at making technology easy enough for everybody else. One example was a VCR (notorious for being hard to even set the digital clock on it). He had one on hand, and as he whipped himself into a frenzy, he suddenly somehow was holding a sledgehammer, and beat the living hell out of that cursed device.

That certainly got my attention, and after talking to him in office hours, he somehow invited me to join his research group. Since then, I've always been most motivated by making hard things easy for everyone.

Randy's group was focused on making developing 3D experiences and VR easy for high school-aged kids to develop, and we were doing a lot of Python, back when it was on Version 1.1. It was early days, and Guido was only a couple of hours from us.

Guido's initial name for his language was ABC, which he had built to be a teaching language. But, it was clear early on that readability could be done well enough to be both beginner-friendly and general purpose.

Then, I fell in love with programming language design and compilers, thanks to classes with Paul Reynolds. That's been my primary technical passion sense, even though I have spent my career in security (you'll soon see I took every opportunity I could to make every problem a compiler problem).

As much as I loved Python for being egalitarian, I had some early experiences that showed me quite clearly that strong static typing is a huge boon to building reliable software.

So, for more than 30 years now, I've been wanting to make something that, to me would be a spiritual successor to Python, addressing the things that, with 20/20 hind-sight, would have been better.

My personal mission is broader than just the language though. I have always thought, programming should be something anyone with a good idea and some determination could master well enough to see their ideas come to life.

# The school of life

After college, I had four kids. I've seen the same thing play out with each of them:

1. They'd want to create something they'd be proud of (almost always a game).
2. I'd do what I could to help guide them down a gentle learning curve, and work with them.
3. They'd work really hard and archieve some results I'd find objectively impressive for a beginner.
4. Unfortunately, their exemplars were so far beyond what their hard work produced that they were not proud.
5. Worse, they intuitively could tell that it would take decades of hard work for them to get to where they wanted to be.

I probably would have had the same reaction as a kid, had I grown up when they did. Thankfully for me, I grew up in an era where most games were coppied out of the back of a magazine.

The games I revered were Infocom games, particularly Zork I-III, and to some lesser extent the original Sierra games, with their blocky 8-bit graphics. For my son, it was Legend of Zelda: Breath of the Wild and Hollow Knight.

People are goal-oriented, and tend to love to have long-term goals (true expertise), but there has to be a clear path of incremental wins. Otherwise, if our long-term goals do not appear achievable, we'll reform them, and drop the short-term goals that were building us to that.

A good onramp for programming needs to give people quick wins that are meaningful to them, and make it intuitively obvious that they'll be able to incrementally achieve more things in which they can take pride.

For a very long time, my vision was focused on providing a more achievable onramp for kids:

- A *general purpose* language as easy to write as Python originally  was, but with automatic static checking wherever possible, with a lot of automation to make it easy (like type inferencing).
- Make common things so simple they don't need code, just   configuration.
- Work with others to build out a bunch of exemplar entry points
  (mainly games) that people could customize, with explicit user
  experiences for common customizations.
- Somehow figure out how to ensure a massive free asset catalog so
  that people don't have to be artists to build something they we
  proud of (this is the part of the plan where I would have consulted the underpants gnomes).

AI makes a lot of this unnecessary, which I'll discuss below. But first, let's finish my backstory.

# What have I been doing about it?
I've been working toward that vision for a long time. And in moments where I haven't been working on the vision, I've still been working around it, making every problem I can a compiler problem.

Coming out of college in 1998, I went into security. My boss had co-authored a book on Java security, and handed me a big grant project that failed before it began because they'd figured out quickly their original proposal wasn't going to work.

They didn't want to lose the grant, and my boss could sell repurposing the grant for a completely different project, so I had full reign to turn the project... into a compiler project.

That's how I ended up building the first two static analysis tools for security, one using full program slicing techniques that were too heavy-weight at the time for programs of any real size, and another that was on the other end of the spectrum, that was far more practical in most places.

Then, I got my own DARPA grant to build a language extension to C for security, and we built an initial version, before I left the job in the fall 2000.

By summer of 2001, I started the first product company for static security analysis, and built another pair of static analysis tools. After that ride, I spent 5 years learning to be an executive at a security company, but inherited two custom programming languages that were criticial tech internally. one of which needed some significant help.

In my next job, I got back to getting my hands dirty. I wrote a couple of domain-specific languages, one called PoliC, which was a fully checked policy language where customers could, in a GUI, express incredibly flexible policies that we could fully check, and show them in real time what their policies would have done.

Then, we could compile the policies to incredibly fast machine code, and dynamically update their policies at the edge, across our entire infrastructure in seconds (we were doing SaaS security for things like email, etc).

I've built a lot of other little DSLs and code analysis tools along the way, but PoliC re-ignited my mission, so by 2012, I was spending my hobby time working on my first *two* iterations of what slowly evolved into n00b, with some assistance from my friend Dr Raid (he built my first garbage collector).

In 2016 I went back into startup life, as CEO, with a bunch of incredibly smart people, so I stayed out of the coding kitchen unless absolutely necessary, but in that 5 years, still managed to do a few compiler projects, including a DSL that drove choose-your-own-adventure interactive demos, where everything was real and users could go off in their own direction.

But I completely set aside that mission, until we sold it, and I retired.

A year of working in isolation though, I went back to work. found myself wanting a configuration language that would maximize our flexibility as we explored a design space. We should be able to declaratively be able to add new flags and options. Our users should just see a config file, but if they wanted more, they should be able to immediately drop into code.

But, it all needed to be verified before production— but we could use a mix of static and dynamic analysis.

I built the first version of that project, `con4m`, in about a week, using a tree interpreter, and whipped out a basic (dumb stack machine) VM shortly after, then started a big string of rewrites.

That work had started out in a language called Nim, which originally felt like it might be a good enough systems language that was easlier in the vein of Python. But there were not only a whole lot of warts, I didn't like the direction, or the BDFL's personal approach to the whole thing.

But there had been reasons why I didn't choose Rust or Zig, so as I kept on iterating, I went back to C, where it was at least easier for me to pull out the set of tools I'd built for myself over the years, many of them around front-end, and analysis.

While `con4m` was work-motivated, being disappointed by Nim left me thinking that, time permitting, `con4m` could iterate into the language I wanted; `n00b` became the name for the rewrite I did in 2024/2025.

While on my second iteration of `n00b`, Baron Oldenburg gave me a particularly big unlock, when he took my API-driven parser evaluator (which was then Earley driven), and used it to build a generic BNF parser that would construct the toolkit's grammar objects, and added some annotations to make it easy to hook up some of my analytics.

C now has a formal grammar, so I then rebuilt my janky C preprocessor on top of it, and then took it a lot farther, spending a lot of time exploring what would be the bare minimum changes to C that would unlock type-safe generic programming; I even joined the C standards committee in late 2025 (but I'm still a lurker, getting my bearings).

In early 2026, I started being able to make AI work for me. It's allowed me to prototype and iterate rapidly.

I've iterated on n00b prototypes a couple of times this year. Plus, my janky C preprocessor became a significant superset of C we're still using internally, we call `ncc` (which I was also using to build a n00b iteration).

There's more to the story, but in the past year, I've thrown myself into all this change, to understand as best I can what AI does well, and what it doesn't.

In short, I now think programming languages won't be as important. But they're still going to be necessary and used, and should adapt to the times.

But I'm always willing to re-think everything. My original mission seems more relevant now than ever, but it's not about just a programming language-- I want to rethink everything about the experience of programming, so that we can better leverage modern AI, to make it far more likely that people with good ideas can get the outcomes they want, without ever reframing their goals as being insurmountable.

# AI doesn't solve programming
AI has probably made much more progress on my goals than would have otherwise happened in the next 50 years without it. 

It's definitely capable of producing some spectacular results on behalf of people who don't know anything about code.

But, while it might be tractable for a novice to build a good game inspired by Flappy Bird, it's hard to imagine them building anything inspired by Silksong.

Similarly, it's easy to see someone who's never tried to program, but is interested in geneology using agents to research a family tree, but it's hard to imagine them building a better Ancestry.com, no matter how good their ideas are to improve it.

Sure, agents are better programmers than many professionals. But their biggest benefit it speed-- they can execute on requirements far faster than a human, and they can rerieve knowledge far faster than a human.

But agents aren't any better at mind reading than any other developer has ever been. They can be made to see a big picture, but they can't keep it as a compass when they need to be down in the details.

Programming is still currently an art, where the developer is now essentially the development manager, trying to make sure the team does the right things in the right way, where we don't just deliver on short term goals, we do it in a way that isn't going to hamstring our long term goals.

Right now, I see plenty of agentic code going to same way as lots of software development in the Waterfall era.

A lot of time and attention goes into building something that is, on first blush, incredibly impressive.

But then, when the manager thinks they're at about 90%, a massive game of "whack-a-mole" begins. People start noticing problems, and seeming to fix one problem, will cause other problems to pop up soon after, with no end in sight.

Generally, this is the point where the manager realizes there are fundamental architectural issues that aren't easily going to be fixed.

Once the realization sets in that it's probably better to start over, but even then there's little assurance of a significantly better outcome, people tend to get despondant, and often give up.

This is basically the same effect I've seen with each of my four kids, before AI. After working hard, they got to milestones that I, as an expert, knew were impressive for a beginner. But in their head, they realized they were such a long way from something they'd be proud of, they gave up.

As a species, we're goal oriented, but if our goals are too hard to achieve, we tend to find different goals.

It's true that modern AI is an amazing tool that goes a lot farther towards my goals than I would have thought possible.

But, they're not only mind readers, they are very myopic. My mental model is, they're like the character Leonard Shelby in the Christopher Nolan classic, Memento. Smart and capable, but lose the plot so quickly, they can easily end up in the wrong direction, focused on the wrong things.

This is the problem space I'm going to be working on the rest of my life. Whenever I decide to stop working at my current job, I'm not going to go to another job-- I plan on the n00b project of being the embodiment of my mission, and think there's enough to explore, and enough innovation to do, to scratch the work itch more than well enough for the rest of my days!

That is, if what I'm doing here gets significant traction in any way that endures, while I'll set up succession plans, I do intend to be a BDFL (benevolent dictator for life), as Guido originally was for Python (In fact, I think him stepping back significantly impacted language choices in a way I wouldn't have wanted to see).
