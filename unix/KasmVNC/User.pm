package KasmVNC::User;

use strict;
use warnings;
use v5.10;

sub new {
    my ($class, $args) = @_;

    my $self = bless {
      name => $args->{name},
      permissions => $args->{permissions}
    }, $class;
}

sub permissionsExplanation {
  my $self = shift;

  my %permissionExplanations = ("w" => "can use keyboard and mouse",
    "o" => "can add/remove users",
    "" => "can only view");
  foreach (qw(ow wo)) {
    $permissionExplanations{$_} = "can use keyboard and mouse, add/remove users";
  }

  $self->{permissions} =~ s/r//g;
  $permissionExplanations{$self->{permissions}};
}

sub name {
  my $self = shift;

  $self->{name};
}

sub permissions {
  my $self = shift;

  $self->{permissions};
}

sub isOwner {
  my $self = shift;

  $self->permissions() =~ /o/;
}

sub toString {
  my $self = shift;

  $self->name() . " (" . $self->permissionsExplanation() . ")";
}

1;
