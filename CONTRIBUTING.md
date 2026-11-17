# Contributing to Ventanal

The server and firmware core are GPL-3.0-or-later. Test changes locally with
`cd server && .venv/bin/python -m pytest -q tests` and
`make -C firmware/test test`. Keep private settings, credentials, device
identifiers, production signing keys and serial logs out of contributions.

## Individual contributor licence agreement

This project-specific agreement draws on the structure of the
[Apache Individual Contributor License Agreement, version 2.2](https://www.apache.org/licenses/icla.pdf).
The additional commercial relicensing permission below is specific to Ventanal;
this is not the Apache agreement or an Apache endorsement.

A **Contribution** is an original change you intentionally submit for inclusion
in Ventanal. **Owner** means Stephen de la Vega. You retain copyright in your
Contribution. Acceptance does not transfer ownership.

By explicitly accepting this agreement for a Contribution, you grant the Owner
an irrevocable, worldwide, perpetual, nonexclusive, royalty-free copyright
licence to use, reproduce, modify, distribute and sublicense that Contribution.
You expressly authorize the Owner to relicense it and derivative works under
other licences, including proprietary and commercial terms, without further
permission or payment. This allows the Owner to offer future extras separately
while keeping the published core open source.

You also grant the Owner and recipients of the Contribution a royalty-free
patent licence covering claims you control necessarily infringed by the
Contribution alone or its combination with Ventanal as submitted. This patent
licence terminates for a recipient who brings patent litigation alleging that
the Contribution infringes a patent.

You confirm you have authority to make these grants, including any required
employer consent. Identify third-party material and its licence separately;
do not promise relicensing rights you do not hold. Contributions are provided
without warranties or an obligation to provide support. Tell the Owner if you
learn that a representation in your acceptance was inaccurate.

Before a contribution is accepted, include this explicit statement in its pull
request, identifying your contribution and public name:

> I have read and accept the Ventanal Individual Contributor Licence Agreement
> in CONTRIBUTING.md for this contribution. I have authority to grant its rights.

The maintainer records acceptance with the pull request. Do not post addresses,
private signatures or other unnecessary personal data. Bug reports and questions
do not require a CLA. Existing third-party code keeps its original licence;
this agreement does not retroactively relicense anyone else's work.
