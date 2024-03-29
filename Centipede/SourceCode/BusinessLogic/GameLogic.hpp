#ifndef GAME_LOGIC_HPP
#define GAME_LOGIC_HPP
#include "MenuLogic.hpp"
#include "../Input/Keylistener.hpp"
#include "../Input/IInputBufferReader.hpp"
#include "../GameObjects/SaveState.hpp"
#include "../GameObjects/Bullet.hpp"
#include "../Common/Tuple.hpp"
#include "../Common/IUI.hpp"
#include "../Common/ITheme.hpp"
#include "../Common/Utils.hpp"
#include "../../lib/concurrency_lib.hpp"
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <chrono>
#include <iostream>

enum ScoreType : int
{
    centipedeHit,
    mushroomKill,
    roundEnd
};

class GameLogic
{
    private:
        std::shared_ptr<MenuLogic> menuLogic_ptr;
        std::shared_ptr<IInputBufferReader> inputBuffer_ptr;
        std::shared_ptr<SaveState> saveState_ptr;
        std::unique_ptr<std::thread> gameClock_thread_ptr;
        std::shared_ptr<IUI> ui_ptr;
        std::shared_ptr<ITheme> theme_ptr;
        bool hasDiedInRound;

        // //////////////////////////////////////////////////
        // Additional Methods
        // //////////////////////////////////////////////////

        /**
         * Starts an endless-loop in this thread and runs the game on the current SafeState.
         * To stop the game, it eather needs to be lost or interrupted by calling breakGame() from another thread.
         */
        void gameLoop()
        {
            auto saveState_ptr = this->saveState_ptr;
            auto inputBuffer_ptr = this->inputBuffer_ptr;
            auto settings_ptr = saveState_ptr->getSettings();
            auto gameClock = startGameClock(settings_ptr->getGameTickLength());
            // Outer game loop.
            while(this->alive())
            {
                // Start a new Round
                this->startNextRound(saveState_ptr);

                // Play through the round
                while(this->alive() && this->continueRound(saveState_ptr->getCentipedes()))
                {
                    saveState_ptr->incrementGameTick();
                    // Await next game tick.
                    gameClock->await();

                    // Do the calculations.
                    this->handlePlayerControlledEntities(inputBuffer_ptr, saveState_ptr);
                    this->handleCentipedes(saveState_ptr);
                    this->handleGlobalCollisions(saveState_ptr);

                    // Print the current state to the UI.
                    this->printGame(saveState_ptr, settings_ptr);

                    // Break the game if necessary.
                    this->breakGameIfNecessary(inputBuffer_ptr, gameClock);
                }

                if(!this->hasDiedInRound)
                {
                    this->increaseScore(ScoreType::roundEnd);
                }
                else
                {
                    // Delay after the starship got hit.
                    auto delayLength = settings_ptr->getLiveLostBreakTime();
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayLength));
                }
            }

            if(this->saveState_ptr->getLives() <= 0)
            {
                this->loseGame();
            }

            this->waitForGameClock();
        }

        /**
         * Stops the game with the next gametick and returns the current Safe State.
         */
        void breakGameIfNecessary(std::shared_ptr<IInputBufferReader> inputBuffer_ptr, std::shared_ptr<Signal> gameClock)
        {
            if(!inputBuffer_ptr->getAndResetBreakoutMenu())
            {
                return;
            }

            auto delayFunction = [gameClock]()
            {
                gameClock->await();
            };
            auto resume = this->menuLogic_ptr->runBreakoutMenu(delayFunction);
            if(resume)
            {
                return;
            }

            // Game was ended -> Kill player to show result screen
            while(this->alive())
            {
                this->loseLive();
            }
        }

        /**
         * Starts the clock in a separate thread.
         */
        std::shared_ptr<Signal> startGameClock(int gameTickLength)
        {
            if(gameClock_thread_ptr != nullptr)
            {
                // already running!
                throw std::logic_error("Game Clock already running.");
            }
            auto gameClock = std::make_shared<Signal>();
            this->gameClock_thread_ptr = std::make_unique<std::thread>(&GameLogic::executeGameClock, this, gameTickLength, gameClock);
            return gameClock;
        }

        /**
         * Runs the clock, that outputs the game tick signals to the given signal.
         */
        void executeGameClock(int gameTickLength, std::shared_ptr<Signal> gameClock)
        {
            while(this->alive())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(gameTickLength));
                gameClock->signal();
            }
        }

        /**
         * The clock is ended by this->isRunning() returning false;
         */
        void waitForGameClock()
        {
            // reset the clock.
            this->gameClock_thread_ptr->join();
            this->gameClock_thread_ptr = nullptr;
        }

        /**
         * Threadsafe check, wheather the game should continue running.
         * Also returns false if the player has no lives left.
         */
        bool alive()
        {
            if(this->saveState_ptr->getLives() > 0)
            {
                return true;
            }
            return false;
        }

        /**
         * Prints the safeState to the UI.
         */
        void printGame(std::shared_ptr<SaveState> saveState_ptr, std::shared_ptr<CentipedeSettings> settings_ptr)
        {
            this->ui_ptr->displayImage(*saveState_ptr, *settings_ptr, *(this->theme_ptr));
        }

        /**
         * Determines wheather a path with the given slowdown should be executed within the current gametick.
         */
        bool executePathForGametick(int gameTick, int moduloSlowdown)
        {
            return gameTick % moduloSlowdown == 0;
        }

        // //////////////////////////////////////////////////
        // High Level Logic Methods
        // //////////////////////////////////////////////////

        /**
         * Handles all starship and bullet actions.
         * This is path 1, executed after a constant gametick delay.
         */
        void handlePlayerControlledEntities(std::shared_ptr<IInputBufferReader> inputBuffer_ptr, std::shared_ptr<SaveState> saveState_ptr)
        {
            auto settings_ptr = saveState_ptr->getSettings();
            auto starshipModuloGametickSlowdown = settings_ptr->getStarshipModuloGametickSlowdown();
            auto currentGameTick = saveState_ptr->getGameTick();
            if(!executePathForGametick(currentGameTick, starshipModuloGametickSlowdown)){
                // Player controlled entities won't move this gametick-> skip path.
                return;
            }

            auto starship_ptr = saveState_ptr->getStarship();
            auto bullets_ptr = saveState_ptr->getBullets();
            auto mushroomMap_ptr = saveState_ptr->getMushroomMap();
            spawnBulletIfNecessary(inputBuffer_ptr, starship_ptr, bullets_ptr);
            moveBullets(bullets_ptr);
            collideBulletsMushrooms(bullets_ptr, mushroomMap_ptr);
            moveStarshipIfNecessary(inputBuffer_ptr, starship_ptr, mushroomMap_ptr);
        }
        
        /**
         * Handles centipede movement.
         * This is path 2, executed after a varying gametick delay.
         */
        void handleCentipedes(std::shared_ptr<SaveState> saveState_ptr)
        {
            auto currentGameTick = saveState_ptr->getGameTick();
            auto currentCentipedeModuloGametickSlowdown = saveState_ptr->getCurrentCentipedeModuloGametickSlowdown();
            if(!executePathForGametick(currentGameTick, currentCentipedeModuloGametickSlowdown)){
                // Centiepedes won't move this gametick-> skip path.
                return;
            }

            auto centipedes_ptr = saveState_ptr->getCentipedes();
            auto mushroomMap_ptr = saveState_ptr->getMushroomMap();
            auto settings_ptr = saveState_ptr->getSettings();
            moveCentipedes(centipedes_ptr, mushroomMap_ptr, settings_ptr);
        }

        /**
         * Handles collisions with objects of both pathes at once: bullet-centipede and player-centipede.
         */
        void handleGlobalCollisions(std::shared_ptr<SaveState> saveState_ptr)
        {
            auto settings_ptr = saveState_ptr->getSettings();
            auto currentCentipedeModuloGametickSlowdown = saveState_ptr->getCurrentCentipedeModuloGametickSlowdown();
            auto starshipModuloGametickSlowdown = saveState_ptr->getSettings()->getStarshipModuloGametickSlowdown();
            auto currentGameTick = saveState_ptr->getGameTick();

            // Collision can only be skipped, if neither path was executed.
            if(!executePathForGametick(currentGameTick, starshipModuloGametickSlowdown)
               && !executePathForGametick(currentGameTick,currentCentipedeModuloGametickSlowdown)){
                return;
            }

            auto centipedes_ptr = saveState_ptr->getCentipedes();
            auto bullets_ptr = saveState_ptr->getBullets();
            auto mushroomMap_ptr = saveState_ptr->getMushroomMap();
            auto starship_ptr = saveState_ptr->getStarship();
            
            this->collideBulletsCentipedes(centipedes_ptr, bullets_ptr, mushroomMap_ptr);
            this->collidePlayerCentipedes(centipedes_ptr, starship_ptr);
        }

        /**
         * Determines wheather the round continues or a new has to be started.
         */
        bool continueRound(std::shared_ptr<std::vector<CentipedeHead>> centipedes_ptr)
        {
            // A round continues, while there are still centipedes left.
            return centipedes_ptr->size() > 0;
        }

        /**
         * Adjusts centipede-lenght and speed and spawns a new centipede to start the next round.
         */
        void startNextRound(std::shared_ptr<SaveState> saveState_ptr)
        {
            saveState_ptr->incrementCurrentRound();
            
            auto currentRound = saveState_ptr->getCurrentRound();
            auto settings_ptr = saveState_ptr->getSettings();

            // Calculate and set new slowdown.
            auto currentSlowdown = this->calculateCentipedeSlowdown(settings_ptr, currentRound);
            saveState_ptr->setCurrentCentipedeModuloGametickSlowdown(currentSlowdown);

            // Calculate Size.
            auto currentSize = this->calculateCentipedeSize(settings_ptr, currentRound);

            // Evaluate initial position and movement
            auto movingDirection = this->getRandomCentipedeMovingDirection();
            auto line = settings_ptr->getCentipedeSpawnLine();
            auto column = settings_ptr->getCentipedeSpawnColumn();

            CentipedeHead newCentipede(line, column, movingDirection, settings_ptr, currentSize);
            saveState_ptr->getCentipedes()->push_back(newCentipede);
        }

        int calculateCentipedeSlowdown(std::shared_ptr<CentipedeSettings> settings_ptr, int currentRound)
        {
            auto initialSlowdown = settings_ptr->getInitialCentipedeModuloGametickSlowdown();
            auto numberOfSpeedups = currentRound / settings_ptr->getCentipedeSpeedIncrementRoundModuloSlowdown();
            auto currentSlowdown = initialSlowdown - (numberOfSpeedups * settings_ptr->getCentipedeSpeedIncrementAmount());
            return currentSlowdown;
        }

        int calculateCentipedeSize(std::shared_ptr<CentipedeSettings> settings_ptr, int currentRound)
        {
            auto initialSize = settings_ptr->getInitialCentipedeSize();
            auto numberOfSizeIncrements = currentRound / settings_ptr->getCentipedeSizeIncrementRoundModuloSlowdown();
            auto currentSize = initialSize + (numberOfSizeIncrements * settings_ptr->getCentipedeSizeIncrementAmount());
            return currentSize;
        }

        CentipedeMovingDirection getRandomCentipedeMovingDirection()
        {
            auto goLeft = rollRandomWithChance(1,2);
            if(goLeft) 
            {
                return CentipedeMovingDirection::cLeft;
            }
            // go right.
            return CentipedeMovingDirection::cRight;
        }

        // //////////////////////////////////////////////////
        // Low Level Logic Methods
        // //////////////////////////////////////////////////

        /**
         * Increases the score according to the type this is called for.
         */
        void increaseScore(ScoreType type)
        {
            auto saveState_ptr = this->saveState_ptr;
            auto settings_ptr = saveState_ptr->getSettings();
            switch(type)
            {
                case centipedeHit:
                {
                    saveState_ptr->addToScore(settings_ptr->getPointsForCentipedeHit());
                    break;
                }
                case mushroomKill:
                {
                    saveState_ptr->addToScore(settings_ptr->getPointsForMushroomKill());
                    break;
                }
                case roundEnd:
                {
                    saveState_ptr->addToScore(settings_ptr->getPointsForRoundEnd());
                    break;
                }
            }
        }

        /**
         * Spawns a bullet if required button was pressed.
         */
        void spawnBulletIfNecessary(std::shared_ptr<IInputBufferReader> inputBuffer_ptr, 
                                    std::shared_ptr<Starship> starship_ptr, 
                                    std::shared_ptr<std::vector<Bullet>> bullets_ptr)
        {
            auto shot = inputBuffer_ptr->getAndResetShot();
            if(shot)
            {
                auto newBullet_ptr = starship_ptr->shoot();
                bullets_ptr->push_back(*newBullet_ptr);
            }
        }

        /**
         * Moves all bullets one line up.
         */
        void moveBullets(std::shared_ptr<std::vector<Bullet>> bullets_ptr)
        {
            auto bullet_ptr = bullets_ptr->begin();
            while(bullet_ptr != bullets_ptr->end())
            {
                auto hasMoved = bullet_ptr->move();
                if(!hasMoved)
                {
                    // Bullet has reached top.
                    bullet_ptr = bullets_ptr->erase(bullet_ptr);
                    continue;
                }
                bullet_ptr++;
            }
        }

        /**
         * Takes Care of Collisions between bullets and mushrooms.
         */
        void collideBulletsMushrooms(std::shared_ptr<std::vector<Bullet>> bullets_ptr,
                                     std::shared_ptr<MushroomMap> mushroomMap_ptr)
        {
            // no simple for loop because vector may be edited while looping through.
            auto bullet_ptr = bullets_ptr->begin();
            while(bullet_ptr != bullets_ptr->end())
            {
                if(mushroomMap_ptr->collide(*bullet_ptr))
                {
                    // Check if Mushroom was killed
                    if(mushroomMap_ptr->getMushroom(bullet_ptr->getPosition().getLine(), bullet_ptr->getPosition().getColumn()) == 0)
                    {
                        this->increaseScore(ScoreType::mushroomKill);
                    }
                    // Collision bullet & mushroom -> remove bullet.
                    bullet_ptr = bullets_ptr->erase(bullet_ptr);
                    // no increment here, since bullet_ptr already points to the following element.
                    continue;
                }
                // No collision, bullet remains in list.
                // check next bullet.
                bullet_ptr++;
            }
        }

        /**
         * Moves the starship if any direction was set by button press.
         */
        void moveStarshipIfNecessary(std::shared_ptr<IInputBufferReader> inputBuffer_ptr,
                                     std::shared_ptr<Starship> starship_ptr,
                                     std::shared_ptr<MushroomMap> mushroomMap_ptr)
        {
            auto direction = inputBuffer_ptr->getAndResetDirection();
            if(direction == Direction::none)
            {
                // no direction was picked.
                return;
            }

            // valid direction was picked.
            starship_ptr->move(direction, *mushroomMap_ptr);
        }

        // //////////////////////////////////////////////////

        /**
         * Moves all centipedes if possible.
         */
        void moveCentipedes(std::shared_ptr<std::vector<CentipedeHead>> centipedes_ptr,
                            std::shared_ptr<MushroomMap> mushroomMap_ptr,
                            std::shared_ptr<CentipedeSettings> settings_ptr)
        {
            auto centipede_ptr = centipedes_ptr->begin();
            while(centipede_ptr != centipedes_ptr->end())
            {
                centipede_ptr->move(*mushroomMap_ptr, *centipedes_ptr, settings_ptr);
                centipede_ptr++;
            }
        }

        // //////////////////////////////////////////////////

        /**
         * Handles collisions between bullets and centipedes.
         */
        void collideBulletsCentipedes(std::shared_ptr<std::vector<CentipedeHead>> centipedes_ptr,
                                      std::shared_ptr<std::vector<Bullet>> bullets_ptr,
                                      std::shared_ptr<MushroomMap> mushroomMap_ptr)
        {
            auto centipede_ptr = centipedes_ptr->begin();
            while(centipede_ptr < centipedes_ptr->end())
            {
                // Indicator wheather the head was hit.
                bool headHit = false;
                // Check bullets.
                // No simple "for" loop because vector may be edited while looping through.
                auto bullet_ptr = bullets_ptr->begin();
                while(bullet_ptr != bullets_ptr->end())
                {
                    auto collisionResult = centipede_ptr->collide(*bullet_ptr, mushroomMap_ptr);
                    auto hitIndicator = collisionResult.getItem1();
                    auto splitOfTail_ptr = collisionResult.getItem2();
                    if(hitIndicator == CentipedeHit::noHit)
                    {
                        // Nothing left to do, just continue checking the others.
                        ++bullet_ptr;
                        continue;
                    }

                    // Bullet has hit -> remove from list.
                    bullet_ptr = bullets_ptr->erase(bullet_ptr);
                    // Update score
                    this->increaseScore(ScoreType::centipedeHit);

                    // Create new centipede from split of tail if necessary.
                    if(splitOfTail_ptr != nullptr)
                    {
                        auto splitOfBody_ptr = std::reinterpret_pointer_cast<CentipedeBody>(splitOfTail_ptr);
                        CentipedeHead newCentipedeFromSplitOfTail(splitOfBody_ptr);
                        // Need to recreate the iterator after adding a new centipede.
                        auto diff = centipede_ptr - centipedes_ptr->begin();
                        centipedes_ptr->push_back(newCentipedeFromSplitOfTail);
                        centipede_ptr = centipedes_ptr->begin() + diff;
                    }

                    if(hitIndicator == CentipedeHit::tailHit)
                    {
                        // Nothing left to do, just continue checking the others.
                        // bullet_ptr already points to next item.
                        continue;
                    }

                    // The head of the Centipede was hit -> return true and do NOT continue checking more bullets.
                    headHit = true;
                    break;
                }

                if(headHit)
                {
                    // Head needs to be removed.
                    centipede_ptr = centipedes_ptr->erase(centipede_ptr);
                    continue;
                }

                // No hit or only tail hit -> continue regulary.
                ++centipede_ptr;
            }
        }

        /**
         * Handles collisions between centipedes and the starship.
         */
        void collidePlayerCentipedes(std::shared_ptr<std::vector<CentipedeHead>> centipedes_ptr,
                                     std::shared_ptr<Starship> starship_ptr)
        {
            auto starshipPosition = starship_ptr->getPosition();
            for(auto centipede : *centipedes_ptr)
            {
                if(!centipede.isAtPosition(starshipPosition))
                {
                    // No collision, game continues running.
                    continue;
                }

                // Collision player & centipede -> lose game.
                this->loseLive();
            }
        }

        /**
         * Decreases player health by 1, kills all centipedes and lets the round end without getting points.
         */
        void loseLive()
        {
            // Decrease health.
            this->saveState_ptr->loseLive();
            // This makes shure that no points for the round end are gained.
            this->hasDiedInRound = true;
            // Remove all enemies.
            this->saveState_ptr->getCentipedes()->clear();
        }

        /**
         * Displays the Game Over screen.
         */
        void loseGame()
        {
            std::string title = "Game Over";
            std::vector<std::string> text;
            text.push_back("Your score was " + std::to_string(this->saveState_ptr->getScore()));
            std::vector<std::string> options;
            this->ui_ptr->displayMenu(title, ConsoleColour::Red, text, options, -1, *(this->theme_ptr), *(this->saveState_ptr->getSettings()));
        }

    public:
        GameLogic(std::shared_ptr<IInputBufferReader> inputBuffer_ptr,
                  std::shared_ptr<IUI> ui_ptr,
                  std::shared_ptr<ITheme> theme_ptr,
                  std::shared_ptr<MenuLogic> menuLogic_ptr)
        {
            this->menuLogic_ptr = menuLogic_ptr;
            this->inputBuffer_ptr = inputBuffer_ptr;

            this->ui_ptr = ui_ptr;
            this->theme_ptr = theme_ptr;

            this->gameClock_thread_ptr = nullptr;
        }

        // //////////////////////////////////////////////////
        // Control Methods
        // //////////////////////////////////////////////////

        /**
         * Starts a new Game.
         */
        void startNew()
        {
            auto settings_ptr = std::make_shared<CentipedeSettings>();
			auto bullets_ptr = std::make_shared<std::vector<Bullet>>();
			auto starship_ptr = std::make_shared<Starship>(settings_ptr->getInitialStarshipLine(),
                                                           settings_ptr->getInitialStarshipColumn(),
                                                           settings_ptr);
			auto mushroomMap_ptr = std::make_shared<MushroomMap>(settings_ptr);
			auto centipedes_ptr = std::make_shared<std::vector<CentipedeHead>>();
			int currentCentipedeModuloGametickSlowdown = settings_ptr->getInitialCentipedeModuloGametickSlowdown();
            int currentRound = 0;
            int score = 0;
            int lives = settings_ptr->getInitialPlayerHealth();
            auto newState = std::make_shared<SaveState>(settings_ptr, 
                                                        bullets_ptr,
                                                        starship_ptr,
                                                        mushroomMap_ptr,
                                                        centipedes_ptr,
                                                        currentCentipedeModuloGametickSlowdown,
                                                        currentRound,
                                                        score,
                                                        lives);
            this->continueGame(newState);
        }

        /**
         * Continues the game of the given Safe State.
         */
        void continueGame(std::shared_ptr<SaveState> state)
        {
            this->saveState_ptr = state;
            this->gameLoop();
        }
};

#endif