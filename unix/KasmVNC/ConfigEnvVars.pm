package KasmVNC::ConfigEnvVars;

use strict;
use warnings;
use v5.10;
use Data::Dumper;

use Exporter;

@KasmVNC::ConfigEnvVars::ISA = qw(Exporter);

our @EXPORT = (
  'OverrideConfigWithConfigEnvVars',
  'CheckForUnsupportedConfigEnvVars'
);

use constant ENV_VAR_OVERRIDE_SETTING => "server.allow_environment_variables_to_override_config_settings";

our @configKeyOverrideDenylist = (
  ENV_VAR_OVERRIDE_SETTING
);

our $logger;
our %prefixedEnvVars;
our %envVarAllowlist;

our $ConfigValue;
our $SetConfigValue;
our $ShouldPrintTopic;
our $SupportedAbsoluteKeys;

sub IsAllowEnvVarOverride {
  my $allowOverride = $ConfigValue->(ENV_VAR_OVERRIDE_SETTING) // "false";
  $allowOverride eq "true";
}

sub OverrideConfigWithConfigEnvVars {
  return unless IsAllowEnvVarOverride();

  %prefixedEnvVars = FetchPrefixedEnvVarsFromEnvironment();
  PrepareEnvVarAllowlist();

  for my $envVarName (sort keys %prefixedEnvVars) {
    my $configKey = $envVarAllowlist{$envVarName};
    next unless defined($configKey);

    my $envVarValue = GetEnvVarValue($envVarName);
    $logger->debug("Overriding $configKey with $envVarName=$envVarValue");
    $SetConfigValue->($configKey, $envVarValue);
  }
}

sub GetEnvVarValue {
  my $envVarName = shift;

  $prefixedEnvVars{$envVarName};
}

sub PrepareEnvVarAllowlist {
  %envVarAllowlist = ();
  my %configKeyOverrideAllowlist = %{ ConfigKeyOverrideAllowlist() };

  for my $configKey (keys %configKeyOverrideAllowlist) {
    my $allowedEnvVarName = ConvertConfigKeyToEnvVarName($configKey);
    $envVarAllowlist{$allowedEnvVarName} = $configKey;
  }
}

sub ConfigKeyOverrideAllowlist {
  my %configKeyOverrideAllowlist = %{ $SupportedAbsoluteKeys->() };
  delete @configKeyOverrideAllowlist{@configKeyOverrideDenylist};

  \%configKeyOverrideAllowlist;
}

sub FetchPrefixedEnvVarsFromEnvironment {
  my %prefixedEnvVars = map { $_ => $ENV{$_} } grep { /^KVNC_/ } keys %ENV;
  PrintPrefixedEnvVars();

  %prefixedEnvVars;
}

sub ConvertConfigKeyToEnvVarName {
  my $configKey = shift;
  my $envVarName = $configKey;

  $envVarName =~ s/\./_/g;
  $envVarName = "KVNC_$envVarName";
  $envVarName = uc $envVarName;
  $logger->debug("$configKey -> $envVarName");

  $envVarName;
}

sub PrintPrefixedEnvVars {
  $logger->debug("Found KVNC_ env vars:");
  for my $envVarName (sort keys %prefixedEnvVars) {
    $logger->debug("$envVarName=$prefixedEnvVars{$envVarName}");
  }
}

sub CheckForUnsupportedConfigEnvVars {
  return unless IsAllowEnvVarOverride();

  my @unsupportedEnvVars =
    grep(!defined($envVarAllowlist{$_}), keys %prefixedEnvVars);

  return if (scalar @unsupportedEnvVars == 0);

  if ($ShouldPrintTopic->("validation")) {
    $logger->warn("Unsupported config env vars found:");
    $logger->warn(join("\n", @unsupportedEnvVars));
    $logger->warn();
  }

  exit 1;
}

1;
