package KasmVNC::Users;

use strict;
use warnings;
use v5.10;
use Data::Dumper;
use List::MoreUtils qw(any);
use KasmVNC::User;

our $vncPasswdBin;
our $logger;

sub new {
    my ($class, $args) = @_;
    my $self = bless {
      passwordFileName => $args->{passwordFileName},
    }, $class;
}

sub loadFrom {
  my ($self, $passwordFileName) = @_;

  my $users = KasmVNC::Users->new({
    passwordFileName => $passwordFileName,
    vncPasswdBin => $vncPasswdBin
  });
  $users->load();

  $users;
}

sub optionsToCliOptions {
  my %options = @_;
  my @cliOptons = ();

  push(@cliOptons, "-u \"@{[$options{username}]}\"");
  if ($options{permissions}) {
    push(@cliOptons, "-" . $options{permissions});
  }
  if ($options{changePermissions}) {
    push(@cliOptons, "-n");
  }

  join " ", @cliOptons;
}

sub runKasmvncpasswd {
  my ($self, $options) = @_;
  my @cliOptions = optionsToCliOptions(%{ $options });

  system("$vncPasswdBin " . join(" ", @cliOptions) .  " " . $self->{passwordFileName});
  $? ? 0 : 1;
}

sub findByPermissions {
  my ($self, $permissions) = @_;

  any { $_->{permissions} =~ /$permissions/ }
    (values %{ $self->{store} });
}

sub fetchUser {
  my ($self, $username) = @_;

  $self->{store}->{$username};
}

sub userExists {
  fetchUser @_;
}

sub addUser {
  my ($self, $username, $permissions) = @_;

  if ($self->userExists($username)) {
    $logger->warn("User $username already exists");
    return;
  }

  $self->runKasmvncpasswd({ username => $username, permissions => $permissions });
}

sub checkUserExists {
  my ($self, $username) = @_;

  unless ($self->fetchUser($username)) {
    die "User \"$username\" doesn't exist";
  }
}

sub addPermissions {
  my ($self, $username, $permissions) = @_;

  $self->checkUserExists($username);

  my $user = $self->fetchUser($username);
  $permissions .= $user->{permissions};

  $self->changePermissions($username, $permissions);
}

sub changePermissions {
  my ($self, $username, $permissions) = @_;

  $self->checkUserExists($username);

  $self->runKasmvncpasswd({ username => $username, permissions => $permissions,
      changePermissions => 1 });
}

sub load {
  my $self = shift;

  $self->{store} = $self->_load();
}

sub reload {
  my $self = shift;

  $self->load();
}

sub count {
  my $self = shift;

  return scalar(keys %{ $self->{store} });
}

sub is_empty {
  my $self = shift;

  $self->count() eq 0;
}

sub _load {
  my $self = shift;

  my $store = {};

  open(FH, '<', $self->{passwordFileName}) or return $store;

  while(<FH>){
    chomp $_;
    my ($name, $__, $permissions) = split(':', $_);
    $store->{$name} = KasmVNC::User->new({
      name => $name,
      permissions => $permissions
    })
  }

  close(FH);

  $store;
}

sub users {
  my $self = shift;

  values %{ $self->{store} }
}

sub toString {
  my $self = shift;

  my @userDescriptions = map { $_->toString() } $self->users();
  join "\n", @userDescriptions;
}

1;
