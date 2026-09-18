# Viewtrious MSIX feasibility package

This parallel package does not replace the native custom installer. It uses development identity placeholders until Microsoft Partner Center supplies the production package identity and publisher values.

Build an unsigned package:

```powershell
.\tools\Build-MSIX.ps1
```

The default output is `out\msix\viewtrious-<version>-x64.msix`. `MakeAppx.exe` validates the manifest and package structure while packing it.

For a local sideload test, create a development certificate whose subject exactly matches the manifest publisher placeholder:

```powershell
$cert = New-SelfSignedCertificate -Type Custom -KeyUsage DigitalSignature `
  -Subject 'CN=Viewtrious Development' -CertStoreLocation 'Cert:\CurrentUser\My' `
  -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') `
  -FriendlyName 'Viewtrious MSIX Development'
$password = Read-Host 'PFX password' -AsSecureString
Export-PfxCertificate -Cert $cert -FilePath '.\viewtrious-dev.pfx' -Password $password
Export-Certificate -Cert $cert -FilePath '.\viewtrious-dev.cer'
```

Trust is an explicit user action. To trust only for the current user:

```powershell
Import-Certificate -FilePath '.\viewtrious-dev.cer' -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople'
```

Build and sign a separate local-test package with SHA-256:

```powershell
.\tools\Build-MSIX.ps1 -SignForLocalTest -CertificatePath '.\viewtrious-dev.pfx' -CertificatePassword $password
```

The signed output is `out\msix\viewtrious-<version>-x64-localtest.msix`. Never commit the PFX or other private-key material. Microsoft Store submission packages are signed by the Store and must use the Partner Center identity values instead of these development placeholders.
