# Contribution guidelines

First, n00b still is pre-launch. If you stumbled onto this from outside the company, we are happy to engage; reach out to me directly at john@crashoverride.com.

In general, feature work should be written up and discussed, with BDFL approval before any PR.

Contributing bug fixes does not need pre-discussion.

However, contributions need to be in accordance with the AI policy, below.

## License policy

To making any contribution to this project, you must agree to the following terms and conditions, that govern all contributions:

1. You must have all rights and permissions necessary to make the contribution.
2. Your contribution must be MIT-licensed.
3. Your contribution must not be subject to any intellectual property restrictions of any kind:
   a) You must attest that the code does not, to your knowledge, vilolate any IP restrictions.
   b) If there is any intellectual property owned by you or your employer, your contribution must assign worldwide, perpetual rights for any use, without any restriction or cost. Your contribution implies willingness, but such assignment is dependent on your contribution being accepted.
4. You abide by our AI policy, interaction / community guidelines, both below.   

## AI policy

This project's explicit goals involve providing an on-ramp to learning, and as such, anyone deemed a maintainer should endeavour to spend some non-trivial portion of their project time shepherding people who are motivated to contribute, as long as the contributor's motivation is focused on the project's overall growth, and where appropriate, their own education and growth.

That is, we do not expect contributions to come only from people who are well-qualified to make them. However, any contributor should be expecting to learn from making the contribution, with AI a tool to assist. Meaning, it is not in any way acceptable to expect that a human can kick off a contribution, delegating all work, understanding and engagement to an agent.

We do believe that, going forward, the vast majority of development will heavily involve AI.

However, AI does have practical limits. This project specifically cares about the following:

- Its documentation and communication skills are currently so far from best-of-breed as to be wholly unacceptable.

- Agents, by their nature, are incapable of accountability.

While we are welcoming of AI involvement in the development process, all contributions must follow these guidelines:

1. There must be an accountable human for the contribution. That human    should take full ownership of the contribution, in the sense that they should ensure every line of code meets project standards, and feel comfortable that they can personally defend each line of code.

2. No comments or documentation of any sort may be produced by agents. You must, nevertheless, provide expected documentation, and be explicitly responsible for every word.

3. Contributions must be to the highest standards of clarify, quality  and performance, as judged solely by the project maintainers.

Generally, we expect people leveraging AI for contributions should be working to ensure they have enough expertise to have full mastery of the contribution.

Additionally, even though communication is hard, we require humans to be responsible for effective communication.

## Interaction guidelines

For contributions that appear (in their subjective judgement) to be wholly unacceptable relative to our policies, or are, in the sole view of maintainers, not appropriate for the language, they will be closed with a terse indication of their reason, and should NOT engage on such issues.

This is a matter of being respectful of everyone's time-- being a maintainer is a significant volunteer commitment, whereas it is easy to produce something with best intentions, that otherwise is not of sufficient quality.

Potential contributors can respectfully ask for feedback, but should expect that feedback will only be provided if the maintainer who closed the item has the time and inclination to address it.

If the appropriate project maintainer, in their subjective opinion, is not comfortable with a contribution, but thinks it might eventually lead to an accepted contribution, they are expected to use their best judgement to determine how to proceed.

Specifically, when contributions may (potentially after much iteration) be deemed acceptable, maintainers will decide whether they are willing to shepherd a contribution.

For in-progress contributions, the maintainer, will endeavor to engage as time permits-- providing respectful feedback with concrete recommendations and next steps.

At any point during the process, if they no longer wish to shepherd a contribution, whether due to lack of time, a determination that the contribution is unlikely to be accepted, or to engagement being unconstructive, they may tersely state their reason, in their sole discretion, and end engagement.

Clarification may be requested once, but after the maintainer responds, that response is definitive and closes all discussion.

In all communication, everyone is expected to be clear, respectful and constructive, even when that courtesy is not being reciprocated.

However, such communication is always considered unacceptable harassment of people choosing to spend their time for the greater good (maintainers or not).

Therefore, in no case should an individual expect a reminder or warning. Violations should be acted upon promptly, and need not merit any comment or rationale whatsoever.

Any repeated harassment resulting from action may result in a ban from any community involvement.

## AI and code review

Let's face it, code review has long been considered valuable, and yet
rarely gets done in a way that provides a lot of value.

Most developers with expertise know they should be spending
significant time digging in on code reviews. But it's often a daunting
task that would take away from other important items that are more
personally rewarding.

"LGTM" often means, I didn't review it, nor am I going to unless
there's an issue, so merge it."

And when people try to do more, what often happens is, developers give
work a cursory read, and either do some style nitpicking, or at least
dig for one or two real issues, before stopping.

When we simply instruct an agent to "review that PR", they often:

1. Are likely to make several claims that are based on a faulty
understanding of the project, because their attention is scoped to the
PR, and they don't bring enough of a deep overall understanding.

2. Reasonable items aren't always likely to be important items.

3. Even if they get the problem right, recommendations are also not
necessarily the right thing.

4. They too can nitpick.

5. They can tend toward unreadable slop.

Still, even with poor agent-based review, I often find there are at
least a couple of items that were worth time and attention.

And, as someone looking for feedback, I often would prefer some
feedback, even if it's AI slop, to basically no feedback.

Once we go public, contributions are all expected to get code
reviews. A different maintainer (someone with commit access) will be
expected to supervise a review:

1. They may, of course, do the review manually, but are expected to be
   reasonably comprehensive.

2. AI reviews are allowed *IF* they are prompted with specific
   guidance on the kinds of issues that the maintainer is most worried
   about.

3. All reviews must independently build and run the code to spot-check
   functionality.

## Note on the initial use of AI

Until the time where I explicitly have gone public with this project,
I (John Viega), am actively leveraging agents, merging PRs from them
without having yet taken full ownership, per the above guidelines.

Much of the code and tools have an origin long predating my use of AI,
constituting a significant portion of the compiler.

Similarly, the language definition and semantics, and all concepts
central to the project currently stem from me, informed mainly from my
career -- both my experience, and my interaction with humans -- not
agents.

However, I have leveraged agents to iterate and migrate things
quickly. As such, agents have touched every line of code at this
point.

The compiler itself started out a large code base, so I am unlikely to
fully reclaim 'ownership' of what's there.

However:

1. I have a strong view on exactly how all major architectural bits
   should work, in that most of it is either extending code I'd
   originally written myself, or work I've asked for that I would have
   been comfortable writing myself without any outside influence, time
   permitting.

   I strive, in my agent interactions to keep it close to what's
   intended, and architected in a way that seems least likely to
   minimize maintainability issues, while acknowledging that there are
   still likely to still be plenty of regrettable changes, despite
   best efforts.

2. Currently, almost all library code is expected to be ports of
   hand-written code that I did in other languages. Currently, I am
   letting agents do the first pass of such things.

   Before going public however, I will take full ownership of all
   library code written in n00b. Meaning, I will review everything,
   and either refine or hand-rewrite until I am comfortable with
   taking full responsibility for every line of code, including every
   mistake.

3. I am, at this stage, very much a detractor of AI-driven comments
   and documentation. I am making somewhat of an effort to keep ALL
   agent-added comments and docs out of the code base at the
   moment. This is easier to keep on top of for documentation, to be
   sure. Before going public, I will 100% manually review every
   comment, and put in explicit controls to ensure agents cannot sneak
   in comments or documentation without an accountable human.

4. While the compiler will long remain a source of discomfort / risk
   for me, I do expect to spend a lot of time after going public with
   this project on two relevant things; first, investing in guardrails
   to enforce my expectations more thoroughly; and second, manually
   reclaiming the parts of the code I am most concerned about or
   otherwise think are most important.
