package KasmVNC::Config;

use strict;
use warnings;
use v5.10;
use YAML::Tiny;
use Data::Dumper;
use Hash::Merge::Simple;
use KasmVNC::Utils;

our $logger;

sub merge {
  my @configsToMerge = map { $_->{data} } @_;
  my $mergedConfig = Hash::Merge::Simple::merge(@configsToMerge) // {};

  KasmVNC::Config->new({ data => $mergedConfig });
}

sub new {
    my ($class, $args) = @_;
    my $self = bless {
      filename => $args->{filename},
      data => $args->{data}
    }, $class;

    $self->load() if $self->{filename};
    $self;
}

sub load {
  my $self = shift;

  failIfConfigNotReadable($self->{filename});

  $logger->debug("Loading config " . $self->{filename});
  my $yamlDocuments = YAML::Tiny->read($self->{filename});
  unless (defined $yamlDocuments) {
    die "Couldn't load config: $self->{filename}. Probable reason: No newline at end of file\n";
  }

  $self->{data} = $yamlDocuments->[0];
}

sub get {
  my ($self, $absoluteKey) = @_;
  my $path = absoluteKeyToHashPath($absoluteKey);
  my $config = $self->{data};

  my $value = eval "\$config$path";
  return unless defined($value);

  $value;
}

sub set {
  my ($self, $absoluteKey, $value) = @_;
  my $path = absoluteKeyToHashPath($absoluteKey);
  my $config = $self->{data};

  eval "\$config$path = \$value";
}

sub exists {
  my ($self, $absoluteKey) = @_;
  my $path = absoluteKeyToHashPath($absoluteKey);
  my $config = $self->{data};

  eval "exists \$config$path";
}

sub delete {
  my ($self, $absoluteKey) = @_;
  my $path = absoluteKeyToHashPath($absoluteKey);
  my $config = $self->{data};

  eval "delete \$config$path";
}

sub isEmpty {
  my ($self, $absoluteKey) = @_;
  my $path = absoluteKeyToHashPath($absoluteKey);
  my $config = $self->{data};

  $self->exists($absoluteKey) && isBlank($self->get($absoluteKey));
}

sub absoluteKeyToHashPath {
  my $absoluteKey = shift;

  my @keyParts = split(/\./, $absoluteKey);
  @keyParts = map { "->{\"$_\"}" } @keyParts;
  join "", @keyParts;
}

sub failIfConfigNotReadable {
  my $config = shift;

  -r $config || die "Couldn't load config: $config";
}

1;
