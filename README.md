# CrossPoint Reader ++

This firmware is based on the [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) for the XTEINK X4, a great piece of software by Dave Allie and others

Unfortunately the official repository suffers from too many good ideas floating around and a lack of clear governance how to deal 
with these contributions, so it's lacking fundamental fixes for a proper reading experience (rendering issues, sub-par sync 
capabilities with KOReader, a popular multi-platform open-source epub reader)

Therefore this branch focuses on real fixes and real improvements while trying to keep up to pace with developments in the main branch.

# What's different

- Proper KOReader Snychronisation (including https TLS OOM fix)
- Fixes for a lot of css rendering issues
- Additional sleep screens support (information overlay, transparent pictures over current reader screen)
- Clock-Support 
- Weather information panel
- Multiple under-the-hood performance improvements
- Book information screen
- Reading ruler
- ...
