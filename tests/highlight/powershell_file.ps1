using namespace System.Collections.Generic

enum Status {
    Draft
    Published
}

class Article {
    [string] $Title
    [Status] $Status = [Status]::Draft

    Article([string] $title) {
        $this.Title = $title
    }

    [string] Render() {
        return "[$($this.Status)] $($this.Title)"
    }
}

function Get-ArticleSummary {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, ValueFromPipeline)]
        [Article] $Article,
        [switch] $Uppercase
    )

    process {
        $text = $Article.Render()
        if ($Uppercase -and $null -ne $text) {
            $text = $text.ToUpperInvariant()
        }
        [pscustomobject]@{
            Title = $Article.Title
            Text = $text
            Active = $true
        }
    }
}

$template = @"
PowerShell here-string
with $($env:USER) interpolation.
"@

[Article]::new("Syntax highlighting") | Get-ArticleSummary -Uppercase

<# A multiline
   PowerShell comment. #>

