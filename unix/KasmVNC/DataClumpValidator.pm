package KasmVNC::DataClumpValidator;

use strict;
use warnings;
use v5.10;
use Data::Dumper;

sub new {
    my ($class, $args) = @_;
    my $self = bless {
    }, $class;
}

sub validate {
  my $self = shift;
  $self->{cliOption} = shift;

  if ($self->isDataClump() && !$self->isWhole()) {
    $self->{cliOption}->addErrorMessage($self->errorMessage());
  }
}

# private

sub isWhole {
  my $self = shift;

  my $numberOfValues = scalar $self->{cliOption}->configValues();
  return 1 if $numberOfValues == 0;

  scalar @{ $self->{cliOption}->{configKeys} } == $numberOfValues;
}

sub isDataClump {
  my $self = shift;

  scalar(@{ $self->{cliOption}->{configKeys} }) > 1;
}

sub errorMessage {
  my $self = shift;

  my $configKeys = join ", ", @{ $self->{cliOption}->configKeyNames() };

  "$configKeys: either all keys or none must be present";
}

1;
