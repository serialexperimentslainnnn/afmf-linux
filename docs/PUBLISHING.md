# Publishing checklist (maintainer)

What only the repository owner can do, in order. Everything else is in the repository.

## Once, after the first push

1. **Pages**: Settings → Pages → Source "Deploy from a branch", branch `main`, folder `/docs`;
   custom domain `afmf-linux.digitalexperiments.dev` (`docs/CNAME`), DNS: a CNAME to
   `serialexperimentslainnnn.github.io` **without a proxy in front** (Cloudflare grey cloud)
   until GitHub has issued the certificate and "Enforce HTTPS" is on; a proxied record hides the
   CNAME from GitHub's check and the certificate never comes. The site builds with GitHub's own
   Jekyll (3.10, plugin allowlist: seo-tag, sitemap, feed). Check
   `https://afmf-linux.digitalexperiments.dev/sitemap.xml` and `/robots.txt`.
   `https://serialexperimentslainnnn.github.io/afmf-linux/` redirects there.
2. **About** (repository sidebar, gear icon):
   - Description: `AMD Fluid Motion Frames for Linux: open-source Vulkan frame generation layer (AFMF) for any game — Proton, DXVK, vkd3d-proton, native. Optical flow interpolation, half-frame pacing, RADV/RDNA. No kernel module, no Mesa patch.`
   - Website: `https://afmf-linux.digitalexperiments.dev/`
   - Topics: `amd afmf fluid-motion-frames frame-generation frame-gen vulkan vulkan-layer linux-gaming proton steam dxvk vkd3d-proton radv mesa rdna4 rdna3 fidelityfx optical-flow interpolation c`
3. **Social preview**: Settings → General → Social preview → upload `docs/assets/og.png`
   (GitHub does not take it from the repository).
4. **Security**: Settings → Code security → enable private vulnerability reporting, Dependabot
   alerts and secret scanning with push protection.
5. **Environment `release`** (created by `packaging/release-key.sh`): Settings → Environments →
   `release` → add yourself as required reviewer, so a tag cannot publish without your approval.
   Secrets there: `RELEASE_GPG_KEY`, `RELEASE_GPG_PASSPHRASE`.
6. **Branch ruleset** on `main`: require the `ci` check, require signed commits, no force push.
7. Optional, if `gh gpg-key add` failed: add `packaging/afmf-linux-release-key.asc` at
   github.com/settings/keys so signatures made with the release key show as verified.

## Every release

1. Local gates on a machine with a GPU (CI cannot run them): `ctest` (3 tests), the ASan build,
   the smoke test, `-fanalyzer`, ShellCheck. Update `CHANGELOG.md` and `CMakeLists.txt` version.
2. `git tag -s vX.Y.Z -m "afmf-linux X.Y.Z"` and `git push origin vX.Y.Z`.
3. `release.yml` builds the tarball, RPM, DEB and Arch package, signs them with the release key,
   attests provenance and drafts the GitHub release with the changelog section. Approve the
   `release` environment when asked, then publish the draft.
4. Verify: `gh release view vX.Y.Z`, download one asset and `gpg --verify` it with
   `packaging/afmf-linux-release-key.asc`; `gh attestation verify <asset> --repo serialexperimentslainnnn/afmf-linux`.
5. AUR (your account with the SSH key registered at aur.archlinux.org → My Account). The first
   time, `git clone ssh://aur@aur.archlinux.org/afmf-linux.git` creates the package. Then:
   `sha256sum` of the release tarball into `packaging/arch/PKGBUILD` (`pkgver`, `sha256sums`),
   `.SRCINFO` regenerated (`makepkg --printsrcinfo > .SRCINFO` on Arch, or by hand), commit here,
   copy both files into the AUR clone, commit, `git push origin master`. The AUR accepts only the
   `master` branch and those two files.

## Search presence

- Google Search Console: add the **Domain** property `afmf-linux.digitalexperiments.dev` (verify
  with the TXT record it gives you, at the DNS provider), submit `sitemap.xml`, request indexing
  of `/` and `/install/`.
- Bing Webmaster Tools: import from Search Console.
- Post the release where Linux gamers read: r/linux_gaming, GamingOnLinux (they cover this kind
  of project), Phoronix forums, the CachyOS and Bazzite forums. Link the site, not the repo: the
  site carries the structured data and the numbers.
