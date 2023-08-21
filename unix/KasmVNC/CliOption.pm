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

our $fetchValueSub;
$KasmVNC::CliOption::dataClumpValidator = KasmVNC::DataClumpValidator->new();
@KasmVNC::CliOption::isActiveCallbacks = ();

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

  push @KasmVNC::CliOption::isActiveCallbacks, $callback;
}

sub isActiveByCallbacks {
  my $self = shift;

  all { $_->($self) } @KasmVNC::CliOption::isActiveCallbacks;
}

sub makeKeysWithValuesAccessible {
  my $self = shift;

  foreach my $name (@{ $self->configKeyNames() }) {
    my $value = $self->fetchValue($name);
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
  $self->interpolateEnvVars($value);
}

sub interpolateEnvVars {
  my $self = shift;
  my $value = shift;

  return $value unless defined($value);

  while ($value =~ /\$\{(\w+)\}/) {
    my $envValue = $ENV{$1};
    $value =~ s/\Q$&\E/$envValue/;
  }

  $value;
}

sub errorMessages {
  my $self = shift;

  join "\n", @{ $self->{errors} };
}

# private

sub isValid {
  my $self = shift;

  $self->validate() unless $self->{validated};

  $self->isNoErrorsPresent();
}

sub validate {
  my $self = shift;

  $self->validateDataClump();
  $self->validateConfigValues();

  $self->{validated} = 1;
}

sub isNoErrorsPresent {
  my $self = shift;

  scalar @{ $self->{errors} } == 0;
}

sub validateDataClump {
  my $self = shift;

  $KasmVNC::CliOption::dataClumpValidator->validate($self);
}

sub configValues {
  my $self = shift;

  map { $self->fetchValue($_->{name}) } @{ $self->{configKeys} };
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

sub addErrorMessage {
  my ($self, $errorMessage) = @_;

  push @{ $self->{errors} }, $errorMessage;
}

sub validateConfigValues {
  my $self = shift;

  map { $_->validate($self) } @{ $self->{configKeys} };
}

sub fetchValue {
  my $self = shift;

  &$fetchValueSub(shift);
}

1;
