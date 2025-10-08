package KasmVNC::ConfigSetting;

use strict;
use warnings;
use v5.10;

use KasmVNC::SettingValidation;

our @ISA = ('KasmVNC::SettingValidation');

our $ConfigValue;

sub new {
  my ($class, $args) = @_;
  my $self = bless {
    configKey => $args->{configKey},
    errors => []
  }, $class;
}

sub toValue {
  my $self = shift;

  $self->deriveValue();
}

sub deriveValue {
  my $self = shift;

  my $configKeyName = $self->{configKey}->{name};
  my $value = $ConfigValue->($configKeyName);
  interpolateEnvVars($value);
}

sub configKeyNames {
  my $self = shift;

  [$self->{configKey}->{name}];
}

# private

sub validate {
  my $self = shift;

  $self->validateConfigValue();

  $self->{validated} = 1;
}

sub validateConfigValue {
  my $self = shift;

  $self->{configKey}->validate($self);
}

1;
