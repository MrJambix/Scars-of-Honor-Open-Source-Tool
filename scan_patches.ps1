param([string]$Dump)
$lines = Get-Content $Dump
$inCore = $false
$owner = ""
$patterns = 'IsLocked|GetCooldown|GetDamage|GetMaxHealth|GetMana|GetStamina|GetCritChance|GetAttackSpeed|GetJumpHeight|GetGravity|CanFly|IsGodMode|TakeDamage|ApplyDamage|IsInvulnerable|CanCast|IsOnCooldown|CanCraft|CanLoot|CanGather|GetGatherTime|GetCastTime|GetDodge|GetParry|GetBlockChance|GetArmor|GetResist|IsStunned|IsRooted|IsSilenced|NoClip|FlySpeed|GetMoveSpeed|GetRunSpeed|GetWalkSpeed|GetMoveAcceleration|GetMaxHp|GetMaxMana|GetCharacterSpeed|GetSwimSpeed|HasEnough|CanAfford|IsRequired|IsPlayerLocked|IsAvailable|IsUnlocked|GetMoveAcceleration|MovementSpeedMultiplier|get_Speed|get_Health|get_Mana|get_Stamina'
$out = New-Object System.Collections.ArrayList
for ($i=0; $i -lt $lines.Length; $i++) {
    $L = $lines[$i]
    if ($L -match '^// (\S+)\s+\[Code\.Core\.dll\]') { $owner = $Matches[1]; $inCore = $true; continue }
    if ($L -match '^// \S+\s+\[(?!Code\.Core\.dll)') { $inCore = $false; continue }
    if ($inCore -and ($L -match "^\s*($patterns)\w*\(")) {
        [void]$out.Add("$owner :: $($L.Trim())")
    }
}
"TOTAL=$($out.Count)"
$out
