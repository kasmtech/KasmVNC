package KasmVNC::TextUI;

use strict;
use warnings;
use v5.10;
use Data::Dumper;

@KasmVNC::TextUI::ISA = qw(Exporter);

our @EXPORT = ('Prompt', 'askUserToChooseOption');

sub askUserToChooseOption {
  my %args = @_;
  my $banner = $args{banner};
  my $prompt = $args{prompt};
  my $options = $args{options};

  my $userInput;
  my $i = 1;
  my %numberedOptions = map { $i++ => $_ } @$options;

  while (1) {
    say $banner;

    printOptions(\%numberedOptions);

    $userInput = Prompt($prompt . ": ");
    last if $numberedOptions{$userInput};

    say "Invalid choice: '$userInput'";
  }

  $numberedOptions{$userInput};
}

sub printOptions {
  my $choices = shift;

  foreach my $choiceNumber (sort keys %$choices) {
    say "[$choiceNumber] " . $choices->{$choiceNumber};
  }
  print "\n";
}

sub Prompt {
  my $prompt = shift;

  print($prompt);
  my $userInput = <STDIN>;
  $userInput =~ s/^\s+|\s+$//g;

  return $userInput;
}

1;
