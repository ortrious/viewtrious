# Viewtrious MSIX packaging

MSIX is a parallel distribution path. The existing custom installer remains the direct/manual distribution package.

## Development package

Build the local-test identity without signing:

```powershell
.\tools\Build-MSIX.ps1
```

Identity: `Viewtrious.Development` / `CN=Viewtrious Development`. Output:

```text
out\msix\development\viewtrious-<version>-x64-development.msix
```

Add `-SignForLocalTest -CertificatePath <PFX> -CertificatePassword <SecureString>` to create the separate `-development-localtest.msix` artifact. The certificate subject must exactly match the manifest publisher.

## Microsoft Store package

Build the unsigned Partner Center submission package:

```powershell
.\tools\Build-MSIX.ps1 -Store
```

The stable identity values come from Partner Center **Product identity** and are kept in the packaging script for reproducible builds:

```text
Name:                 ortrious.viewtrious
Publisher:            CN=3A400F6A-D8BA-4784-84CE-0351CAC0C7F3
PublisherDisplayName: ortrious
DisplayName:          viewtrious
```

Output:

```text
out\msix\store\viewtrious-<version>-x64-store.msix
```

The Store package uses an isolated Release build configured with `VIEWTRIOUS_STORE_BUILD=ON`; CMake forcibly disables `VIEWTRIOUS_ENABLE_UPDATE_CHECKS`. Microsoft Store performs production signing after certification. Do not sign the submission artifact with the development certificate.

To make a separately named Store-identity package for local testing, supply a PFX whose Subject exactly matches the Store Publisher:

```powershell
$password = Read-Host 'PFX password' -AsSecureString
.\tools\Build-MSIX.ps1 -Store -SignForLocalTest `
  -CertificatePath '<path-to-store-localtest.pfx>' `
  -CertificatePassword $password
```

This creates `viewtrious-<version>-x64-store-localtest.msix`. The script does not create or trust certificates and does not install the package.

Both package modes declare the Microsoft `Microsoft.VCLibs.140.00.UWPDesktop` framework required by the dynamically linked MSVC runtime. The Store installs this dependency automatically; a sideload test machine must already have the matching x64 framework package.

## Verify identity

Inspect a generated package without installing it:

```powershell
MakeAppx.exe unpack /p '<package.msix>' /d '<empty-inspection-directory>' /o
Get-Content '<empty-inspection-directory>\AppxManifest.xml'
```

Confirm the manifest identity and four-part version match the intended mode. Never upload a `Viewtrious.Development` package to Partner Center.
