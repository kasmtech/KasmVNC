package KasmVNC::CliOption;

use strict;
use warnings;
use v5.10;
use List::Util qw(first);
use List::MoreUtils qw(all);
use Switch;
use Data::Dumper;

use KasmVNC::DataClumpValidator;
use KasmVNC::Utils;
use KasmVNC::SettingValidation;

our @ISA = ('KasmVNC::SettingValidation');

our $ConfigValue;
our $dataClumpValidator = KasmVNC::DataClumpValidator->new();
our @isActiveCallbacks = ();

sub new {
    my ($class, $args) = @_;
    my $self = bless {
      name => $args->{name},
      configKeys => $args->{configKeys},
      deriveValueSub => $args->{deriveValueSub} || sub {
        my $self = shift;
        my @values = @{ listify($self->configValues()) };

        @values = map { deriveBoolean($_) } @values;

        join ",", @values;
      },
      isActiveSub => $args->{isActiveSub} || sub {
        my $self = shift;

        scalar $self->configValues() > 0;
      },
      toStringSub => $args->{toStringSub}  || sub {
        my $self = shift;

        my $derivedValue = $self->deriveValue();
        if (defined($derivedValue)) {
          return "-$self->{name} " . "'$derivedValue'";
        }

        "-$self->{name}";
      },
      errors => []
    }, $class;
}

sub activate {
  my $self = shift;

  $self->makeKeysWithValuesAccessible();
}

sub beforeIsActive {
  my $callback = shift;

  push @isActiveCallbacks, $callback;
}

sub isActiveByCallbacks {
  my $self = shift;

  all { $_->($self) } @isActiveCallbacks;
}

sub makeKeysWithValuesAccessible {
  my $self = shift;

  foreach my $name (@{ $self->configKeyNames() }) {
    my $value = $ConfigValue->($name);
    $self->{$name} = $value if defined($value);
  }
}

sub isActive {
  my $self = shift;

  $self->isActiveByCallbacks() && $self->{isActiveSub}->($self);
}

sub toString {
  my $self = shift;

  return unless $self->isActive();

  $self->{toStringSub}->($self);
}

sub toValue {
  my $self = shift;

  return unless $self->isActive();

  $self->deriveValue();
}

sub deriveValue {
  my $self = shift;

  my $value = $self->{deriveValueSub}->($self);
  interpolateEnvVars($value);
}

# private

sub validate {
  my $self = shift;

  $self->validateDataClump();
  $self->validateConfigValues();

  $self->{validated} = 1;
}

sub validateDataClump {
  my $self = shift;

  $dataClumpValidator->validate($self);
}

sub configValues {
  my $self = shift;

  map { $ConfigValue->($_->{name}) } @{ $self->{configKeys} };
}

sub configValue {
  my $self = shift;

  die "Multiple or no config keys defined for $self->{name}"
    if (scalar @{ $self->{configKeys} } != 1);

  @{ listify($self->configValues()) }[0];
}

sub configKeyNames {
  my $self = shift;

  my @result = map { $_->{name} } @{ $self->{configKeys} };
  \@result;
}

sub hasKey {
  my $self = shift;
  my $configKey = shift;

  first { $_ eq $configKey } @{ $self->configKeyNames() };
}

sub validateConfigValues {
  my $self = shift;

  map { $_->validate($self) } @{ $self->{configKeys} };
}

1;
